#include "audio_decoder.h"

// Phase 28: compile-time probe for avcodec_{get,set}_decoder_state —
// added in 2026-04-23 (patch 0009-aac-decoder-state.patch). Same idea
// as VOD_HAVE_ENCODER_STATE_API in audio_encoder.c.
#if defined(LIBAVCODEC_VERSION_INT) && \
    LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 30, 100)
#define VOD_HAVE_DECODER_STATE_API 1
#else
#define VOD_HAVE_DECODER_STATE_API 0
#endif

#if VOD_HAVE_DECODER_STATE_API
extern int avcodec_get_decoder_state(AVCodecContext *avctx,
                                     uint8_t **buf, size_t *size);
extern int avcodec_set_decoder_state(AVCodecContext *avctx,
                                     const uint8_t *buf, size_t size);
#endif

// globals
static const AVCodec *decoder_codec = NULL;
static bool_t initialized = FALSE;

void
audio_decoder_process_init(vod_log_t* log)
{
	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 18, 100)
		avcodec_register_all();
	#endif

	decoder_codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (decoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_decoder_process_init: failed to get AAC decoder, audio decoding is disabled");
		return;
	}

	initialized = TRUE;
}

static vod_status_t
audio_decoder_init_decoder(
	audio_decoder_state_t* state,
	media_info_t* media_info)
{
	AVCodecContext* decoder;
	int avrc;

	if (media_info->codec_id != VOD_CODEC_ID_AAC)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_init_decoder: codec id %uD not supported", media_info->codec_id);
		return VOD_BAD_REQUEST;
	}

	// init the decoder	
	decoder = avcodec_alloc_context3(decoder_codec);
	if (decoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_init_decoder: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	state->decoder = decoder;	
	
	decoder->codec_tag = media_info->format;
	decoder->bit_rate = media_info->bitrate;
	decoder->time_base.num = 1;
	decoder->time_base.den = media_info->frames_timescale;
	decoder->pkt_timebase = decoder->time_base;
	decoder->extradata = media_info->extra_data.data;
	decoder->extradata_size = media_info->extra_data.len;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
	av_channel_layout_from_mask(&decoder->ch_layout, media_info->u.audio.channel_layout);
#else
	decoder->channels = media_info->u.audio.channels;
	decoder->channel_layout = media_info->u.audio.channel_layout;
#endif

	decoder->bits_per_coded_sample = media_info->u.audio.bits_per_sample;
	decoder->sample_rate = media_info->u.audio.sample_rate;

	avrc = avcodec_open2(decoder, decoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_init_decoder: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

vod_status_t
audio_decoder_init(
	audio_decoder_state_t* state,
	request_context_t* request_context,
	media_track_t* track,
	int cache_slot_id)
{
	frame_list_part_t* part;
	input_frame_t* last_frame;
	input_frame_t* cur_frame;
	vod_status_t rc;

	if (!initialized)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_decoder_init: module failed to initialize successfully");
		return VOD_UNEXPECTED;
	}

	state->request_context = request_context;

	// init the decoder
	rc = audio_decoder_init_decoder(
		state,
		&track->media_info);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// allocate a frame
	state->decoded_frame = av_frame_alloc();
	if (state->decoded_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_decoder_init: av_frame_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->cur_frame_part = track->frames;

	// audio post-roll: the parser left trail_frame_count extra frames right
	// after frames.last_frame (same contiguous array, single part). Extend this
	// decoder's private copy of the part over them; the track itself is left
	// untouched so every other consumer still sees the exact segment window.
	state->trail_first_frame = NULL;
	state->boundary_state_data = NULL;
	state->boundary_state_size = 0;
	if (track->trail_frame_count > 0 && track->frames.next == NULL)
	{
		state->trail_first_frame = track->frames.last_frame;
		state->cur_frame_part.last_frame = track->frames.last_frame + track->trail_frame_count;
	}

	// calculate the max frame size (over the trail too)
	state->max_frame_size = 0;
	part = &state->cur_frame_part;
	last_frame = part->last_frame;
	for (cur_frame = part->first_frame;; cur_frame++)
	{
		if (cur_frame >= last_frame)
		{
			if (part->next == NULL)
			{
				break;
			}
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
		}

		if (cur_frame->size > state->max_frame_size)
		{
			state->max_frame_size = cur_frame->size;
		}
	}

	// initialize the frame state
	state->cur_frame_pos = 0;
	state->data_handled = TRUE;
	state->frame_started = FALSE;
	state->frame_buffer = NULL;

	// Phase 28: initialise state-in fields to "no pending restore".
	// audio_filter_restore_state will overwrite these in-place if the
	// transport carries a matching decoder blob for this source.
	state->state_in_data = NULL;
	state->state_in_size = 0;
	state->state_restored = 0;

	state->cur_frame = track->frames.first_frame;
	state->dts = track->first_frame_time_offset;

	state->cur_frame_part.frames_source->set_cache_slot_id(
		state->cur_frame_part.frames_source_context,
		cache_slot_id);

	return VOD_OK;
}

void
audio_decoder_set_trail(
	audio_decoder_state_t* state,
	uint32_t trail_frame_count)
{
	if (state->trail_first_frame == NULL)
	{
		return;
	}

	if (trail_frame_count < (uint32_t)(state->cur_frame_part.last_frame - state->trail_first_frame))
	{
		state->cur_frame_part.last_frame = state->trail_first_frame + trail_frame_count;
	}

	if (trail_frame_count == 0)
	{
		state->trail_first_frame = NULL;
	}
}

uint32_t
audio_decoder_get_trail(audio_decoder_state_t* state)
{
	if (state->trail_first_frame == NULL)
	{
		return 0;
	}

	return (uint32_t)(state->cur_frame_part.last_frame - state->trail_first_frame);
}

void
audio_decoder_free(audio_decoder_state_t* state)
{
	// avcodec_free_context, not avcodec_close + av_free: on FFmpeg master
	// avcodec_close is a no-op shim, so only the context struct was freed and
	// the codec's priv_data/internal (~0.5MB for AAC) leaked on every segment.
	// extradata points into the request pool (audio_decoder_init_decoder) and
	// avcodec_free_context would av_freep it, so hand it back first.
	if (state->decoder != NULL)
	{
		state->decoder->extradata = NULL;
		state->decoder->extradata_size = 0;
	}
	avcodec_free_context(&state->decoder);
	av_frame_free(&state->decoded_frame);

	if (state->boundary_state_data != NULL)
	{
		av_free(state->boundary_state_data);
		state->boundary_state_data = NULL;
		state->boundary_state_size = 0;
	}
}

// Send one packet into the decoder and try to receive one frame. Shared
// between the regular decode path and the Phase 28 warm-up-and-re-decode
// dance. `buffer` must remain valid for the duration of the call.
static vod_status_t
audio_decoder_send_and_receive(
	audio_decoder_state_t* state,
	u_char* buffer,
	uint32_t size,
	int64_t dts,
	int64_t pts,
	uint32_t duration,
	int* out_avrc)
{
	AVPacket* input_packet = av_packet_alloc();
	u_char original_pad[VOD_BUFFER_PADDING_SIZE];
	u_char* frame_end;
	int avrc;

	if (input_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_send_and_receive: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	input_packet->data = buffer;
	input_packet->size = size;
	input_packet->dts = dts;
	input_packet->pts = pts;
	input_packet->duration = duration;
	input_packet->flags = AV_PKT_FLAG_KEY;

	av_frame_unref(state->decoded_frame);

	frame_end = buffer + size;
	vod_memcpy(original_pad, frame_end, sizeof(original_pad));
	vod_memzero(frame_end, sizeof(original_pad));

	avrc = avcodec_send_packet(state->decoder, input_packet);
	av_packet_free(&input_packet);
	if (avrc < 0)
	{
		vod_memcpy(frame_end, original_pad, sizeof(original_pad));
		*out_avrc = avrc;
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_send_and_receive: avcodec_send_packet failed %d", avrc);
		return VOD_BAD_DATA;
	}

	avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);
	vod_memcpy(frame_end, original_pad, sizeof(original_pad));
	*out_avrc = avrc;
	return VOD_OK;
}

static vod_status_t
audio_decoder_decode_frame(
	audio_decoder_state_t* state,
	u_char* buffer,
	AVFrame** result)
{
	input_frame_t* frame = state->cur_frame;
	uint32_t frame_size     = frame->size;
	uint32_t frame_duration = frame->duration;
	int64_t frame_dts       = (int64_t)state->dts;
	int64_t frame_pts       = (int64_t)state->dts + frame->pts_delay;
	int avrc;
	vod_status_t rc;

	// Advance stream pos — matches legacy behaviour.
	state->dts += frame->duration;

	rc = audio_decoder_send_and_receive(
		state, buffer, frame_size, frame_dts, frame_pts, frame_duration, &avrc);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// move to the next frame
	state->cur_frame++;
	if (state->cur_frame >= state->cur_frame_part.last_frame &&
		state->cur_frame_part.next != NULL)
	{
		state->cur_frame_part = *state->cur_frame_part.next;
		state->cur_frame = state->cur_frame_part.first_frame;
	}

	state->frame_started = FALSE;

#if VOD_HAVE_DECODER_STATE_API
	// Phase 28: if a state-in blob is pending and the first receive
	// succeeded, che[][] is now allocated and we can restore the
	// overlap. Restore, then re-send the SAME packet so the decoded
	// frame reflects the restored IMDCT overlap. The discarded first
	// decode wasted one IMDCT, but no real-content samples are lost
	// because we re-decode the same source packet.
	if (avrc == 0 &&
	    state->state_in_data != NULL &&
	    state->state_in_size > 0 &&
	    !state->state_restored)
	{
		int set_rc = avcodec_set_decoder_state(state->decoder,
			state->state_in_data, state->state_in_size);
		state->state_restored = 1;
		if (set_rc < 0)
		{
			vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
				"audio_decoder_decode_frame: avcodec_set_decoder_state failed "
				"%d (size=%uz) — continuing with cold-start overlap",
				set_rc, state->state_in_size);
		}
		else
		{
			// Re-send same packet with restored overlap. Re-use the
			// original dts/pts/duration so the encoder pipeline sees the
			// same timestamps.
			int redecode_avrc = 0;
			vod_status_t redecode_rc = audio_decoder_send_and_receive(
				state, buffer, frame_size, frame_dts, frame_pts,
				frame_duration, &redecode_avrc);
			if (redecode_rc != VOD_OK)
			{
				return redecode_rc;
			}
			avrc = redecode_avrc;
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"audio_decoder_decode_frame: restored state and re-decoded "
				"first packet (size=%uz)", state->state_in_size);
		}
	}

	// audio post-roll: the packet just decoded was the last one of the segment
	// window — snapshot the decoder now, before any trail packet touches the
	// IMDCT overlap. This is the state the next segment (which starts at the
	// first trail packet) must restore; see audio_decoder_capture_state.
	if (state->trail_first_frame != NULL &&
	    state->cur_frame == state->trail_first_frame &&
	    state->boundary_state_data == NULL)
	{
		uint8_t* blob = NULL;
		size_t   blob_sz = 0;
		int      grc = avcodec_get_decoder_state(state->decoder, &blob, &blob_sz);

		if (grc >= 0 && blob != NULL && blob_sz > 0)
		{
			state->boundary_state_data = blob;
			state->boundary_state_size = blob_sz;
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"audio_decoder_decode_frame: captured boundary decoder state (%uz bytes)", blob_sz);
		}
		else
		{
			vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
				"audio_decoder_decode_frame: avcodec_get_decoder_state at the window boundary failed %d", grc);
			if (blob != NULL)
			{
				av_free(blob);
			}
		}
	}
#endif

	if (avrc == AVERROR(EAGAIN))
	{
		return VOD_AGAIN;
	}

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_decoder_decode_frame: avcodec_receive_frame failed %d", avrc);
		return VOD_BAD_DATA;
	}

	*result = state->decoded_frame;
	return VOD_OK;
}

vod_status_t
audio_decoder_get_frame(
	audio_decoder_state_t* state,
	AVFrame** result)
{
	u_char* read_buffer;
	uint32_t read_size;
	vod_status_t rc;
	bool_t frame_done;

	for (;;)
	{
		// start a frame if needed
		if (!state->frame_started)
		{
			if (state->cur_frame >= state->cur_frame_part.last_frame)
			{
				return VOD_DONE;
			}

			// start the frame
			rc = state->cur_frame_part.frames_source->start_frame(
				state->cur_frame_part.frames_source_context,
				state->cur_frame,
				NULL);
			if (rc != VOD_OK)
			{
				return rc;
			}

			state->frame_started = TRUE;
		}

		// read some data from the frame
		rc = state->cur_frame_part.frames_source->read(
			state->cur_frame_part.frames_source_context,
			&read_buffer,
			&read_size,
			&frame_done);
		if (rc != VOD_OK)
		{
			if (rc != VOD_AGAIN)
			{
				return rc;
			}

			if (!state->data_handled)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"audio_decoder_get_frame: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			state->data_handled = FALSE;
			return VOD_AGAIN;
		}

		state->data_handled = TRUE;

		if (!frame_done)
		{
			// didn't finish the frame, append to the frame buffer
			if (state->frame_buffer == NULL)
			{
				state->frame_buffer = vod_alloc(
					state->request_context->pool,
					state->max_frame_size + VOD_BUFFER_PADDING_SIZE);
				if (state->frame_buffer == NULL)
				{
					vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
						"audio_decoder_get_frame: vod_alloc failed");
					return VOD_ALLOC_FAILED;
				}
			}

			vod_memcpy(state->frame_buffer + state->cur_frame_pos, read_buffer, read_size);
			state->cur_frame_pos += read_size;
			continue;
		}

		if (state->cur_frame_pos != 0)
		{
			// copy the remainder
			vod_memcpy(state->frame_buffer + state->cur_frame_pos, read_buffer, read_size);
			state->cur_frame_pos = 0;
			read_buffer = state->frame_buffer;
		}

		// process the frame
		rc = audio_decoder_decode_frame(state, read_buffer, result);
		if (rc != VOD_AGAIN)
		{
			return rc;
		}
	}
}

// Phase 28: snapshot the decoder's cross-frame state into an FFSA blob
// for transport to the next segment. Safe to call any time after at
// least one frame has been decoded. Returns VOD_OK + NULL buffer if
// the library lacks the API or the decoder hasn't allocated its
// channel elements yet (benign — caller proceeds without a blob).
vod_status_t
audio_decoder_capture_state(
	audio_decoder_state_t* state,
	u_char** out_data,
	size_t* out_size)
{
	*out_data = NULL;
	*out_size = 0;

	if (state == NULL || state->decoder == NULL)
	{
		return VOD_OK;
	}

#if VOD_HAVE_DECODER_STATE_API
	// audio post-roll: hand over the boundary snapshot (ownership moves to the
	// caller), the live decoder state is past the segment window by now
	if (state->boundary_state_data != NULL)
	{
		*out_data = state->boundary_state_data;
		*out_size = state->boundary_state_size;
		state->boundary_state_data = NULL;
		state->boundary_state_size = 0;
		return VOD_OK;
	}

	if (state->trail_first_frame != NULL)
	{
		// the decoder never reached the boundary (or the snapshot failed), so
		// the live state would describe some other position in the stream —
		// emit nothing rather than a wrong overlap
		vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
			"audio_decoder_capture_state: no boundary snapshot, skipping decoder blob");
		return VOD_OK;
	}

	{
		uint8_t* blob = NULL;
		size_t   blob_sz = 0;
		int      grc;

		grc = avcodec_get_decoder_state(state->decoder, &blob, &blob_sz);
		if (grc >= 0 && blob != NULL && blob_sz > 0)
		{
			*out_data = blob;
			*out_size = blob_sz;
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"audio_decoder_capture_state: captured %uz bytes", blob_sz);
		}
		else
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"audio_decoder_capture_state: avcodec_get_decoder_state=%d", grc);
			if (blob != NULL)
			{
				av_free(blob);
			}
		}
	}
#endif

	return VOD_OK;
}
