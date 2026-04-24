#!/bin/bash
# Phase 28: apply AAC decoder state-transfer additions to an FFmpeg tree
# that already has patches 0001..0008 applied. Run from the FFmpeg root.
set -euo pipefail

# Source path: default to the script's own directory (so it works when
# invoked directly from the nginx-vod-module checkout in a container
# build), but allow override via SRC env var for legacy consumers that
# still pre-stage the files at /tmp/ffmpeg-phase28-files.
SRC="${SRC:-$(cd "$(dirname "$0")" && pwd)}"

# 1) Drop the two new source files into libavcodec/aac/.
cp "$SRC/aacdec_state.c" libavcodec/aac/aacdec_state.c
cp "$SRC/aacdec_state.h" libavcodec/aac/aacdec_state.h

# 2) Add aacdec_state.o to the AAC decoder OBJS list.
if ! grep -q 'aac/aacdec_state.o' libavcodec/aac/Makefile; then
    awk '
        /aac\/aacdec_usac_mps212\.o/ {
            print $0 " \\"
            print "                                        aac/aacdec_state.o"
            next
        }
        { print }
    ' libavcodec/aac/Makefile > libavcodec/aac/Makefile.new
    mv libavcodec/aac/Makefile.new libavcodec/aac/Makefile
fi

# 3) Add FFSA_COMPONENT_AAC_DECODER and decoder atom IDs to state_format.h.
if ! grep -q 'FFSA_COMPONENT_AAC_DECODER' libavcodec/state_format.h; then
    awk '
        /^#define FFSA_COMPONENT_AMIX        0x0003/ {
            print
            print "#define FFSA_COMPONENT_AAC_DECODER 0x0004  /* Phase 28 */"
            next
        }
        /^#define FFSA_AAC_ICS_MAX_SFB/ {
            print
            print ""
            print "/* AAC decoder atom tags (component 0x0004) - Phase 28 */"
            print "#define FFSA_AACDEC_SAVED                0x3001"
            print "#define FFSA_AACDEC_IS_SAVED             0x3002"
            print "#define FFSA_AACDEC_WINDOW_SEQUENCE      0x3003"
            print "#define FFSA_AACDEC_USE_KB_WINDOW        0x3004"
            print "#define FFSA_AACDEC_RANDOM_STATE         0x3005"
            print "#define FFSA_AACDEC_ICS_MAX_SFB          0x3006"
            next
        }
        { print }
    ' libavcodec/state_format.h > libavcodec/state_format.h.new
    mv libavcodec/state_format.h.new libavcodec/state_format.h
fi

# 4) Declare avcodec_{get,set}_decoder_state in avcodec.h, right after
#    avcodec_set_encoder_state's closing paren-semicolon line.
if ! grep -q 'avcodec_get_decoder_state' libavcodec/avcodec.h; then
    awk '
        BEGIN { seen_get = 0; inserted = 0 }
        /^int avcodec_set_encoder_state\(/ { in_set = 1 }
        { print }
        in_set && /size_t size\);/ && !inserted {
            print ""
            print "int avcodec_get_decoder_state(AVCodecContext *avctx,"
            print "                              uint8_t **buf, size_t *size);"
            print "int avcodec_set_decoder_state(AVCodecContext *avctx,"
            print "                              const uint8_t *buf, size_t size);"
            in_set = 0
            inserted = 1
        }
    ' libavcodec/avcodec.h > libavcodec/avcodec.h.new
    mv libavcodec/avcodec.h.new libavcodec/avcodec.h
fi

# 5) Append avcodec_{get,set}_decoder_state dispatch to decode.c.
if ! grep -q 'avcodec_get_decoder_state' libavcodec/decode.c; then
    cat >> libavcodec/decode.c <<'EOF'

/* Phase 28: decoder state snapshot/restore public API. */
#include "config_components.h"
#if CONFIG_AAC_DECODER
#include "aac/aacdec_state.h"
#endif

int avcodec_get_decoder_state(AVCodecContext *avctx,
                              uint8_t **buf, size_t *size)
{
    if (!avctx || !buf || !size)
        return AVERROR(EINVAL);
    *buf = NULL;
    *size = 0;
    if (!av_codec_is_decoder(avctx->codec))
        return AVERROR(EINVAL);
    switch (avctx->codec_id) {
#if CONFIG_AAC_DECODER
    case AV_CODEC_ID_AAC:
        return ff_aac_decoder_get_state(avctx, buf, size);
#endif
    default:
        return AVERROR(ENOSYS);
    }
}

int avcodec_set_decoder_state(AVCodecContext *avctx,
                              const uint8_t *buf, size_t size)
{
    if (!avctx || !buf || !size)
        return AVERROR(EINVAL);
    if (!av_codec_is_decoder(avctx->codec))
        return AVERROR(EINVAL);
    switch (avctx->codec_id) {
#if CONFIG_AAC_DECODER
    case AV_CODEC_ID_AAC:
        return ff_aac_decoder_set_state(avctx, buf, size);
#endif
    default:
        return AVERROR(ENOSYS);
    }
}
EOF
fi

echo "Phase 28 applied successfully."
