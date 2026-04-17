#ifndef __AUDIO_ENCODER_H__
#define __AUDIO_ENCODER_H__

// includes
#include "../media_format.h"
#include <libavcodec/avcodec.h>

// constants
// Modern FFmpeg (>= ~6.0) native AAC encoder declares only AV_SAMPLE_FMT_FLTP
// in its codec->sample_fmts list. Using S16 here makes
// audio_encoder_is_format_supported() return FALSE at startup and disables
// audio encoding. The filter chain (see audio_filter_init_sink) uses this
// macro for the buffersink sample_formats option, so libavfilter will
// auto-insert a format conversion from whatever the decoder produced (the
// AAC decoder already outputs FLTP, so this is usually a no-op). The
// encoder itself does not touch AVFrame buffers directly; it forwards them
// to avcodec_send_frame(), so no byte-width / planar-vs-packed arithmetic
// needs adjusting in audio_encoder.c. AUDIO_ENCODER_BITS_PER_SAMPLE (16)
// remains correct — that is the coded/output bits-per-sample written into
// the MP4 stsd box, not the input sample width.
#define AUDIO_ENCODER_INPUT_SAMPLE_FORMAT (AV_SAMPLE_FMT_FLTP)

//typedefs

// Stateful-audio hook: opaque FFSA blob shuttle.
// state_in: optional pointer to prior segment's serialised encoder state
//           (consumed by avcodec_set_encoder_state after open2). NULL = first
//           segment / upstream 404 / hook disabled.
// state_out: on successful flush, audio_encoder will av_malloc() a fresh blob
//           via avcodec_get_encoder_state and set *state_out_data / *state_out_size.
//           Caller owns the buffer (must av_free it). NULL pointers disable
//           capture.
typedef struct
{
	uint64_t channel_layout;
	uint16_t channels;
	uint32_t sample_rate;
	uint32_t timescale;
	uint32_t bitrate;

	// stateful-audio hook (opt-in; all NULL/0 → pre-hook behavior)
	const u_char* state_in_data;
	size_t        state_in_size;
	u_char**      state_out_data;
	size_t*       state_out_size;
} audio_encoder_params_t;

// functions
void audio_encoder_process_init(
	vod_log_t* log);

vod_status_t audio_encoder_init(
	request_context_t* request_context,
	audio_encoder_params_t* params,
	vod_array_t* frames_array,
	void** result);

void audio_encoder_free(
	void* context);

size_t audio_encoder_get_frame_size(
	void* context);

vod_status_t audio_encoder_write_frame(
	void* context,
	AVFrame* frame);

vod_status_t audio_encoder_flush(
	void* context);

vod_status_t audio_encoder_update_media_info(
	void* context,
	media_info_t* media_info);

#endif // __AUDIO_ENCODER_H__
