/*
 * AAC decoder state snapshot/restore — internal declarations (Phase 28).
 *
 * Mirror of libavcodec/aacenc_state.h for the decoder side. Captures
 * enough cross-frame state that a fresh decoder context, after
 * set_state, emits the same PCM as a continuous decoder would at the
 * same timeline position.
 *
 * For the common AAC-LC case on stem sources (Moises pipeline), the
 * minimal required state is:
 *   - SingleChannelElement.saved[1536]  (IMDCT overlap buffer)
 *   - AACDecContext.is_saved            (overlap valid flag)
 *   - ics.window_sequence[0..1]         (drives IMDCT window choice)
 *   - ics.use_kb_window[0..1]           (drives IMDCT window shape)
 *   - AACDecContext.random_state        (PNS RNG, if any)
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef AVCODEC_AAC_AACDEC_STATE_H
#define AVCODEC_AAC_AACDEC_STATE_H

#include <stdint.h>
#include <stddef.h>

#include "libavcodec/avcodec.h"

/* Public-API backing. Callers: avcodec_{get,set}_decoder_state. */
int ff_aac_decoder_get_state(AVCodecContext *avctx,
                             uint8_t **buf, size_t *size);
int ff_aac_decoder_set_state(AVCodecContext *avctx,
                             const uint8_t *buf, size_t size);

#endif /* AVCODEC_AAC_AACDEC_STATE_H */
