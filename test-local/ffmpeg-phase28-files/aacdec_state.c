/*
 * AAC decoder — state snapshot/restore (Phase 28).
 *
 * Implements avcodec_{get,set}_decoder_state for the native AAC decoder.
 * Serialises the minimal AAC-LC cross-frame state set identified in
 * stateful-aac-validation/research/phase28-aac-decoder-state-scope.md
 * into the FFSA wire format.
 *
 * Atoms captured (component 0x0004, all REQUIRED):
 *   0x3001 FFSA_AACDEC_SAVED           per-SCE 1536 × f32 IMDCT overlap
 *   0x3002 FFSA_AACDEC_IS_SAVED        u8
 *   0x3003 FFSA_AACDEC_WINDOW_SEQUENCE per-SCE 2 × u32
 *   0x3004 FFSA_AACDEC_USE_KB_WINDOW   per-SCE 2 × u8
 *   0x3005 FFSA_AACDEC_RANDOM_STATE    u32
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <string.h>

#include "libavutil/mem.h"
#include "libavutil/error.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/version.h"
#include "libavcodec/aac.h"
#include "libavcodec/state_format.h"

#include "aacdec.h"
#include "aacdec_state.h"

#define AACDEC_COMPONENT_VERSION 0x0001
#define AACDEC_NB_SAVED 1536  /* samples per SCE in overlap buffer */

/* --------------------------------------------- enumerate active SCEs -- */

/* Iterates che[type][id] in a canonical order (SCE before CPE before CCE
 * before LFE, ascending id) and returns SCE count. The per-SCE visitor
 * callback receives the SCE plus its ordinal index. Used for both
 * get_state and set_state to guarantee a stable layout. */
typedef int (*sce_visitor)(AACDecContext *ac, SingleChannelElement *sce,
                           int sce_idx, void *user);

static int for_each_sce(AACDecContext *ac, sce_visitor visit, void *user)
{
    static const int type_order[4] = { TYPE_SCE, TYPE_CPE, TYPE_CCE, TYPE_LFE };
    int t, id, c, sce_idx = 0;
    for (t = 0; t < 4; t++) {
        int type = type_order[t];
        int chans = (type == TYPE_CPE) ? 2 : 1;
        for (id = 0; id < MAX_ELEM_ID; id++) {
            ChannelElement *che = ac->che[type][id];
            if (!che)
                continue;
            for (c = 0; c < chans; c++) {
                SingleChannelElement *sce = &che->ch[c];
                int ret = visit(ac, sce, sce_idx++, user);
                if (ret < 0)
                    return ret;
            }
        }
    }
    return sce_idx;
}

static int count_sce_cb(AACDecContext *ac, SingleChannelElement *sce,
                        int idx, void *user)
{
    (void)ac; (void)sce; (void)idx; (void)user;
    return 0;
}

static int count_sces(AACDecContext *ac)
{
    return for_each_sce(ac, count_sce_cb, NULL);
}

/* --------------------------------------------------------- config hash -- */

static uint64_t compute_config_hash(AVCodecContext *avctx, AACDecContext *ac)
{
    uint8_t buf[128];
    size_t off = 0;
    uint64_t layout_mask;

    ff_ffsa_wr_u32be(buf + off, FFSA_COMPONENT_AAC_DECODER); off += 4;
    ff_ffsa_wr_u32be(buf + off, AACDEC_COMPONENT_VERSION);   off += 4;
    ff_ffsa_wr_u32be(buf + off, (uint32_t)avctx->sample_rate); off += 4;
    ff_ffsa_wr_u32be(buf + off, (uint32_t)avctx->ch_layout.nb_channels); off += 4;
    if (avctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE)
        layout_mask = avctx->ch_layout.u.mask;
    else
        layout_mask = 0;
    ff_ffsa_wr_u64be(buf + off, layout_mask); off += 8;
    ff_ffsa_wr_u32be(buf + off, (uint32_t)avctx->profile); off += 4;
    ff_ffsa_wr_u32be(buf + off, (uint32_t)ac->oc[1].m4ac.object_type); off += 4;
    ff_ffsa_wr_u32be(buf + off, (uint32_t)count_sces(ac)); off += 4;
    ff_ffsa_wr_u32be(buf + off, (uint32_t)ac->is_fixed); off += 4;

    return ff_ffsa_sha256_prefix64(buf, off);
}

static uint32_t compute_build_hash(void)
{
    const char *ident = av_version_info();
    if (!ident)
        return 0;
    return ff_ffsa_crc32((const uint8_t *)ident, strlen(ident));
}

/* ----------------------------------------------------------- snapshot -- */

typedef struct {
    uint8_t *saved_buf;     /* nb_sce × 1536 × f32 */
    uint8_t *ws_buf;        /* nb_sce × 2 × u32 */
    uint8_t *kb_buf;        /* nb_sce × 2 × u8 */
    int err;
} collect_ctx;

static int collect_sce_cb(AACDecContext *ac, SingleChannelElement *sce,
                          int idx, void *user)
{
    collect_ctx *cc = user;
    IndividualChannelStream *ics = &sce->ics;
    int i;
    (void)ac;

    /* saved[1536]: in the decoder build we may be compiling with USE_FIXED
     * under integer, so read the union as a float array only when the
     * context is float. Use ac->is_fixed as the runtime indicator.
     * (In fixed build SingleChannelElement.saved is int, not float.)
     *
     * For the Moises use case we only ship the float decoder; on the
     * fixed decoder we'd need a separate atom — guard below. */
    if (ac->is_fixed) {
        cc->err = AVERROR(ENOSYS);
        return cc->err;
    }

    {
        const float *src = (const float *)sce->saved;
        uint8_t *dst = cc->saved_buf + (size_t)idx * AACDEC_NB_SAVED * 4;
        for (i = 0; i < AACDEC_NB_SAVED; i++)
            ff_ffsa_wr_f32be(dst + i * 4, src[i]);
    }

    ff_ffsa_wr_u32be(cc->ws_buf + (size_t)idx * 2 * 4 + 0,
                     (uint32_t)ics->window_sequence[0]);
    ff_ffsa_wr_u32be(cc->ws_buf + (size_t)idx * 2 * 4 + 4,
                     (uint32_t)ics->window_sequence[1]);
    cc->kb_buf[(size_t)idx * 2 + 0] = ics->use_kb_window[0];
    cc->kb_buf[(size_t)idx * 2 + 1] = ics->use_kb_window[1];
    return 0;
}

int ff_aac_decoder_get_state(AVCodecContext *avctx,
                             uint8_t **out_buf, size_t *out_size)
{
    AACDecContext *ac = avctx->priv_data;
    FFSAWriter w;
    uint16_t flags = 0;
    uint8_t tmp[8];
    int ret, nb_sce;
    collect_ctx cc = { 0 };

    if (!out_buf || !out_size)
        return AVERROR(EINVAL);
    if (!ac || ac->is_fixed)
        return AVERROR(ENOSYS);  /* fixed decoder not supported */
    if (avctx->flags & AV_CODEC_FLAG_BITEXACT)
        flags |= FFSA_FLAG_BITEXACT;

    nb_sce = count_sces(ac);
    if (nb_sce <= 0) {
        /* Decoder not yet initialised / no channel elements allocated.
         * Can happen if get_state is called before any frame decoded. */
        return AVERROR(EAGAIN);
    }

    ret = ff_ffsa_writer_begin(&w,
                               FFSA_COMPONENT_AAC_DECODER,
                               AACDEC_COMPONENT_VERSION,
                               LIBAVCODEC_VERSION_INT,
                               compute_build_hash(),
                               compute_config_hash(avctx, ac),
                               flags);
    if (ret < 0)
        return ret;

    /* Allocate payload buffers once; single pass over all SCEs fills them. */
    {
        uint32_t saved_total = (uint32_t)nb_sce * AACDEC_NB_SAVED * 4;
        uint32_t ws_total    = (uint32_t)nb_sce * 2 * 4;
        uint32_t kb_total    = (uint32_t)nb_sce * 2;
        cc.saved_buf = av_malloc(saved_total);
        cc.ws_buf    = av_malloc(ws_total);
        cc.kb_buf    = av_malloc(kb_total);
        if (!cc.saved_buf || !cc.ws_buf || !cc.kb_buf) {
            ret = AVERROR(ENOMEM); goto fail;
        }

        ret = for_each_sce(ac, collect_sce_cb, &cc);
        if (ret < 0) goto fail;

        /* 0x3001 SAVED */
        ret = ff_ffsa_writer_add_atom(&w, FFSA_AACDEC_SAVED,
                                      FFSA_ATOM_REQUIRED,
                                      cc.saved_buf, saved_total);
        if (ret < 0) goto fail;

        /* 0x3003 WINDOW_SEQUENCE */
        ret = ff_ffsa_writer_add_atom(&w, FFSA_AACDEC_WINDOW_SEQUENCE,
                                      FFSA_ATOM_REQUIRED,
                                      cc.ws_buf, ws_total);
        if (ret < 0) goto fail;

        /* 0x3004 USE_KB_WINDOW */
        ret = ff_ffsa_writer_add_atom(&w, FFSA_AACDEC_USE_KB_WINDOW,
                                      FFSA_ATOM_REQUIRED,
                                      cc.kb_buf, kb_total);
        if (ret < 0) goto fail;
    }

    /* 0x3002 IS_SAVED (u8) */
    tmp[0] = ac->is_saved ? 1 : 0;
    ret = ff_ffsa_writer_add_atom(&w, FFSA_AACDEC_IS_SAVED,
                                  FFSA_ATOM_REQUIRED, tmp, 1);
    if (ret < 0) goto fail;

    /* 0x3005 RANDOM_STATE (u32) */
    ff_ffsa_wr_u32be(tmp, (uint32_t)ac->random_state);
    ret = ff_ffsa_writer_add_atom(&w, FFSA_AACDEC_RANDOM_STATE,
                                  FFSA_ATOM_REQUIRED, tmp, 4);
    if (ret < 0) goto fail;

    ret = ff_ffsa_writer_finish(&w, out_buf, out_size);
    if (ret < 0) goto fail;
    av_free(cc.saved_buf); av_free(cc.ws_buf); av_free(cc.kb_buf);
    return 0;

fail:
    ff_ffsa_writer_abort(&w);
    av_free(cc.saved_buf); av_free(cc.ws_buf); av_free(cc.kb_buf);
    *out_buf = NULL;
    *out_size = 0;
    return ret;
}

/* ------------------------------------------------------------- restore -- */

typedef struct {
    const uint8_t *saved_buf;
    const uint8_t *ws_buf;
    const uint8_t *kb_buf;
} restore_ctx;

static int restore_sce_cb(AACDecContext *ac, SingleChannelElement *sce,
                          int idx, void *user)
{
    restore_ctx *rc = user;
    IndividualChannelStream *ics = &sce->ics;
    int i;

    if (ac->is_fixed)
        return AVERROR(ENOSYS);

    {
        float *dst = (float *)sce->saved;
        const uint8_t *src = rc->saved_buf + (size_t)idx * AACDEC_NB_SAVED * 4;
        for (i = 0; i < AACDEC_NB_SAVED; i++)
            dst[i] = ff_ffsa_rd_f32be(src + i * 4);
    }

    ics->window_sequence[0] =
        (enum WindowSequence)ff_ffsa_rd_u32be(rc->ws_buf + (size_t)idx * 2 * 4 + 0);
    ics->window_sequence[1] =
        (enum WindowSequence)ff_ffsa_rd_u32be(rc->ws_buf + (size_t)idx * 2 * 4 + 4);
    ics->use_kb_window[0] = rc->kb_buf[(size_t)idx * 2 + 0];
    ics->use_kb_window[1] = rc->kb_buf[(size_t)idx * 2 + 1];
    return 0;
}

int ff_aac_decoder_set_state(AVCodecContext *avctx,
                             const uint8_t *buf, size_t size)
{
    AACDecContext *ac = avctx->priv_data;
    FFSAReader r;
    const uint8_t *val;
    uint32_t vlen;
    int ret, nb_sce;
    restore_ctx rc;

    if (!ac || ac->is_fixed)
        return AVERROR(ENOSYS);

    /* pass 1: validate blob integrity. */
    ret = ff_ffsa_reader_validate(&r, buf, size);
    if (ret < 0)
        return ret;

    if (r.header.component_id != FFSA_COMPONENT_AAC_DECODER)
        return AVERROR(EINVAL);
    if (r.header.component_version != AACDEC_COMPONENT_VERSION)
        return AVERROR(EINVAL);
    if (r.header.config_hash != compute_config_hash(avctx, ac))
        return AVERROR(EINVAL);

    nb_sce = count_sces(ac);
    if (nb_sce <= 0)
        return AVERROR(EAGAIN);

    /* pass 1b: required atoms present and correctly sized. */
    {
        struct { uint16_t tag; uint32_t expected; } req[] = {
            { FFSA_AACDEC_SAVED,           (uint32_t)nb_sce * AACDEC_NB_SAVED * 4 },
            { FFSA_AACDEC_IS_SAVED,        1 },
            { FFSA_AACDEC_WINDOW_SEQUENCE, (uint32_t)nb_sce * 2 * 4 },
            { FFSA_AACDEC_USE_KB_WINDOW,   (uint32_t)nb_sce * 2 },
            { FFSA_AACDEC_RANDOM_STATE,    4 },
        };
        size_t k;
        for (k = 0; k < sizeof(req) / sizeof(req[0]); k++) {
            if (!ff_ffsa_reader_find(&r, req[k].tag, &val, &vlen))
                return AVERROR(EINVAL);
            if (vlen != req[k].expected)
                return AVERROR(EINVAL);
        }
    }

    /* pass 2: mutate. */
    ff_ffsa_reader_find(&r, FFSA_AACDEC_SAVED, &val, &vlen);
    rc.saved_buf = val;
    ff_ffsa_reader_find(&r, FFSA_AACDEC_WINDOW_SEQUENCE, &val, &vlen);
    rc.ws_buf = val;
    ff_ffsa_reader_find(&r, FFSA_AACDEC_USE_KB_WINDOW, &val, &vlen);
    rc.kb_buf = val;

    ret = for_each_sce(ac, restore_sce_cb, &rc);
    if (ret < 0)
        return ret;

    ff_ffsa_reader_find(&r, FFSA_AACDEC_IS_SAVED, &val, &vlen);
    ac->is_saved = val[0] ? 1 : 0;

    ff_ffsa_reader_find(&r, FFSA_AACDEC_RANDOM_STATE, &val, &vlen);
    ac->random_state = (int)ff_ffsa_rd_u32be(val);

    return 0;
}
