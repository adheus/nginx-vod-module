#ifndef __AUDIO_DECODER_H__
#define __AUDIO_DECODER_H__

// includes
#include "../media_format.h"
#include <libavcodec/avcodec.h>

// macros
#define audio_decoder_has_frame(decoder) \
	((decoder)->cur_frame < (decoder)->cur_frame_part.last_frame)

// typedefs
typedef struct {
	request_context_t* request_context;
	AVCodecContext* decoder;
	AVFrame* decoded_frame;

	frame_list_part_t cur_frame_part;
	input_frame_t* cur_frame;
	uint64_t dts;

	u_char* frame_buffer;
	uint32_t max_frame_size;
	uint32_t cur_frame_pos;
	bool_t data_handled;
	bool_t frame_started;

	// Phase 28: stateful decoder. If state_in_data is set at init time,
	// the decoder will restore its IMDCT overlap + window-sequence
	// history from the FFSA blob after the first packet is decoded
	// (che[][] must be allocated first), then re-decode that first
	// packet so the emitted frame reflects the restored overlap.
	// state_restored flips from 0 → 1 after the first successful
	// restore-and-redecode. Both state_in pointers may be NULL/0 which
	// means "cold-start decoder, legacy behaviour".
	const u_char* state_in_data;
	size_t        state_in_size;
	int           state_restored;
} audio_decoder_state_t;

// functions
void audio_decoder_process_init(vod_log_t* log);

vod_status_t audio_decoder_init(
	audio_decoder_state_t* state,
	request_context_t* request_context,
	media_track_t* track,
	int cache_slot_id);

void audio_decoder_free(audio_decoder_state_t* state);

vod_status_t audio_decoder_get_frame(
	audio_decoder_state_t* state,
	AVFrame** result);

// Phase 28: snapshot the decoder's current state (post-frame N) into an
// FFSA blob. Caller owns *out_data and must av_free() it. Returns
// VOD_OK with *out_data/size set on success, VOD_OK with NULL/0 if
// the decoder library doesn't support state capture or no frames have
// been decoded yet.
vod_status_t audio_decoder_capture_state(
	audio_decoder_state_t* state,
	u_char** out_data,
	size_t* out_size);

#endif // __AUDIO_DECODER_H__
