#include "audio_encoder.h"
#include "audio_filter.h"

// constants
#define AUDIO_ENCODER_BITS_PER_SAMPLE (16)

// stateful-audio: compile-time probe for encoder state API
// (see stateful-aac-validation/impl/ffmpeg-patches/0003-public-api.patch)
#if defined(LIBAVCODEC_VERSION_INT) && \
    LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 30, 100)
#define VOD_HAVE_ENCODER_STATE_API 1
#else
#define VOD_HAVE_ENCODER_STATE_API 0
#endif

// typedefs
typedef struct
{
	request_context_t* request_context;
	vod_array_t* frames_array;
	AVCodecContext *encoder;

	// stateful-audio output capture (owned by caller once set)
	u_char** state_out_data;
	size_t*  state_out_size;
} audio_encoder_state_t;

// globals
static const AVCodec *encoder_codec = NULL;
static bool_t initialized = FALSE;

static char* aac_encoder_names[] = {
	"libfdk_aac",
	"aac",
	NULL
};


static bool_t
audio_encoder_is_format_supported(const AVCodec *codec, enum AVSampleFormat sample_fmt)
{
	const enum AVSampleFormat *p;

	for (p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++)
	{
		if (*p == sample_fmt)
		{
			return TRUE;
		}
	}

	return FALSE;
}

void
audio_encoder_process_init(vod_log_t* log)
{
	char** name;

	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 18, 100)
		avcodec_register_all();
	#endif

	for (name = aac_encoder_names; ; name++)
	{
		if (*name == NULL)
		{
			vod_log_error(VOD_LOG_WARN, log, 0,
				"audio_encoder_process_init: failed to get AAC encoder, audio encoding is disabled. recompile libavcodec with an aac encoder to enable it");
			return;
		}

		encoder_codec = avcodec_find_encoder_by_name(*name);
		if (encoder_codec != NULL)
		{
			vod_log_error(VOD_LOG_INFO, log, 0,
				"audio_encoder_process_init: using aac encoder \"%s\"", *name);
			break;
		}
	}

	if (!audio_encoder_is_format_supported(encoder_codec, AUDIO_ENCODER_INPUT_SAMPLE_FORMAT))
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_encoder_process_init: encoder does not support the required input format, audio encoding is disabled");
		return;
	}

	initialized = TRUE;
}

vod_status_t
audio_encoder_init(
	request_context_t* request_context,
	audio_encoder_params_t* params,
	vod_array_t* frames_array,
	void** result)
{
	audio_encoder_state_t* state;
	AVCodecContext* encoder;
	int avrc;

	if (!initialized)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_encoder_init: module failed to initialize successfully");
		return VOD_UNEXPECTED;
	}

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_encoder_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// init the encoder
	encoder = avcodec_alloc_context3(encoder_codec);
	if (encoder == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_encoder_init: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	state->encoder = encoder;

	encoder->sample_fmt = AUDIO_ENCODER_INPUT_SAMPLE_FORMAT;
	encoder->time_base.num = 1;
	encoder->time_base.den = params->timescale;
	encoder->sample_rate = params->sample_rate;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
	av_channel_layout_from_mask(&encoder->ch_layout, params->channel_layout);
#else
	encoder->channels = params->channels;
	encoder->channel_layout = params->channel_layout;
#endif

	encoder->bit_rate = params->bitrate;
	encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;		// make the codec generate the extra data

	// stateful-audio: ALWAYS opt into BITEXACT, not only when restoring.
	// Rationale: every segment's encoder must produce the same config_hash
	// for FFSA blob compatibility. If cold-start segments run with
	// BITEXACT=0 and then seg-N+1 sets BITEXACT=1 to restore, seg-N+1's
	// avcodec_set_encoder_state fails with EINVAL (config_hash mismatch)
	// → falls back to fresh encoder → priming silence at that boundary.
	// Must be set BEFORE avcodec_open2 so the opened encoder's extradata
	// and state-format hash are consistent across save and restore paths.
	encoder->flags |= AV_CODEC_FLAG_BITEXACT;

	avrc = avcodec_open2(encoder, encoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_encoder_init: avcodec_open2 failed %d", avrc);
		audio_encoder_free(state);
		return VOD_UNEXPECTED;
	}

	state->request_context = request_context;
	state->frames_array = frames_array;
	state->state_out_data = params->state_out_data;
	state->state_out_size = params->state_out_size;

	// stateful-audio: restore prior segment state, if any
	if (params->state_in_data != NULL && params->state_in_size > 0)
	{
#if VOD_HAVE_ENCODER_STATE_API
		avrc = avcodec_set_encoder_state(encoder,
			params->state_in_data, params->state_in_size);
		if (avrc < 0)
		{
			// restore failed → log + continue with fresh encoder (priming
			// artefact will return for THIS segment only). Correctness > opt.
			vod_log_error(VOD_LOG_WARN, request_context->log, 0,
				"audio_encoder_init: avcodec_set_encoder_state failed %d, "
				"falling back to fresh encoder", avrc);
		}
		else
		{
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"audio_encoder_init: restored encoder state (%uz bytes)",
				params->state_in_size);
		}
#else
		// FFmpeg build predates avcodec_set_encoder_state — noisy warn
		// once per process would be better; for now log at debug level.
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_encoder_init: state_in ignored (ffmpeg lacks state API)");
#endif
	}

	*result = state;

	return VOD_OK;
}

void
audio_encoder_free(
	void* context)
{
	audio_encoder_state_t* state = context;

	if (state == NULL)
	{
		return;
	}
	
	avcodec_close(state->encoder);
	av_free(state->encoder);
}

size_t
audio_encoder_get_frame_size(void* context)
{
	audio_encoder_state_t* state = context;

	if ((state->encoder->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0)
	{
		return 0;
	}

	return state->encoder->frame_size;
}

static vod_status_t
audio_encoder_write_packet(
	audio_encoder_state_t* state,
	AVPacket* output_packet)
{
	input_frame_t* cur_frame;
	vod_status_t rc;
	void* data;

	rc = audio_filter_alloc_memory_frame(
		state->request_context,
		state->frames_array,
		output_packet->size,
		&cur_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	data = (void*)(uintptr_t)cur_frame->offset;
	vod_memcpy(data, output_packet->data, output_packet->size);

	cur_frame->duration = output_packet->duration;
	cur_frame->pts_delay = output_packet->pts - output_packet->dts;

	return VOD_OK;
}

vod_status_t
audio_encoder_write_frame(
	void* context,
	AVFrame* frame)
{
	audio_encoder_state_t* state = context;
	vod_status_t rc;
	AVPacket* output_packet;
	int avrc;

	// send frame
	avrc = avcodec_send_frame(state->encoder, frame);

	av_frame_unref(frame);

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_write_frame: avcodec_send_frame failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	// receive packet
	output_packet = av_packet_alloc();
	if (output_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_write_frame: av_packet_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	// packet data will be allocated by the encoder

	avrc = avcodec_receive_packet(state->encoder, output_packet);

	if (avrc == AVERROR(EAGAIN))
	{
		av_packet_free(&output_packet);
		return VOD_OK;
	}

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_write_frame: avcodec_receive_packet failed %d", avrc);
		av_packet_free(&output_packet);
		return VOD_ALLOC_FAILED;
	}

	rc = audio_encoder_write_packet(state, output_packet);

	av_packet_free(&output_packet);

	return rc;
}

vod_status_t
audio_encoder_flush(
	void* context)
{
	audio_encoder_state_t* state = context;
	AVPacket* output_packet;
	vod_status_t rc;
	int avrc;

	// Phase 23 Candidate A probe: capture state BEFORE drain so we can
	// diff against the post-drain state to see if drain itself mutates
	// any atom values. If so, the mutated atoms are what cause the
	// sub-perceptible boundary blip — state captured at post-drain
	// holds a future-shifted psy snapshot vs what continuous encoder
	// has at the same timeline position.
#if VOD_HAVE_ENCODER_STATE_API
	uint8_t* pre_drain_blob = NULL;
	size_t   pre_drain_sz = 0;
	if (state->state_out_data != NULL && state->state_out_size != NULL)
	{
		int prc = avcodec_get_encoder_state(state->encoder,
			&pre_drain_blob, &pre_drain_sz);
		if (prc == 0 && pre_drain_blob != NULL)
		{
			vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
				"phase23_probe: pre-drain state captured (%uz bytes)",
				pre_drain_sz);
		}
		else
		{
			pre_drain_blob = NULL; pre_drain_sz = 0;
		}
	}
#endif

	avrc = avcodec_send_frame(state->encoder, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_flush: avcodec_send_frame failed %d", avrc);
#if VOD_HAVE_ENCODER_STATE_API
		if (pre_drain_blob != NULL) av_free(pre_drain_blob);
#endif
		return VOD_UNEXPECTED;
	}

	output_packet = av_packet_alloc();
	if (output_packet == NULL) {
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_flush: av_packet_alloc failed");
#if VOD_HAVE_ENCODER_STATE_API
		if (pre_drain_blob != NULL) av_free(pre_drain_blob);
#endif
		return VOD_ALLOC_FAILED;
	}

	for (;;)
	{
		// packet data will be allocated by the encoder, av_packet_unref is always called
		avrc = avcodec_receive_packet(state->encoder, output_packet);
		if (avrc == AVERROR_EOF)
		{
			break;
		}

		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_encoder_flush: avcodec_receive_packet failed %d", avrc);
			av_packet_free(&output_packet);
#if VOD_HAVE_ENCODER_STATE_API
			if (pre_drain_blob != NULL) av_free(pre_drain_blob);
#endif
			return VOD_UNEXPECTED;
		}

		rc = audio_encoder_write_packet(state, output_packet);

		if (rc != VOD_OK)
		{
			av_packet_free(&output_packet);
#if VOD_HAVE_ENCODER_STATE_API
			if (pre_drain_blob != NULL) av_free(pre_drain_blob);
#endif
			return rc;
		}
	}

	av_packet_free(&output_packet);

	// stateful-audio: snapshot encoder state post-flush. Must be AFTER final
	// avcodec_receive_packet returned AVERROR_EOF so the AFQ and planar
	// sample buffers reflect end-of-segment state.
	if (state->state_out_data != NULL && state->state_out_size != NULL)
	{
#if VOD_HAVE_ENCODER_STATE_API
		uint8_t* blob = NULL;
		size_t   blob_sz = 0;
		int      grc = avcodec_get_encoder_state(state->encoder, &blob, &blob_sz);
		if (grc == 0 && blob != NULL && blob_sz > 0)
		{
			// Phase 23 Candidate D probe: walk both pre-drain and
			// post-drain blobs atom-by-atom and report per-atom mutations.
			// Previous byte-range diff only reported the single contiguous
			// diff region at blob end (FRAME_NUM + CRC), missing atoms
			// earlier in the blob that may also mutate (e.g.
			// PLANAR_SAMPLES if drain zeros the lookahead region).
			//
			// FFSA wire format:
			//   [36-byte header] [atoms...] [4-byte CRC]
			//   Each atom = u16 type BE + u16 flags BE + u32 length BE
			//                + payload + pad-to-4-byte
			if (pre_drain_blob != NULL && pre_drain_sz == blob_sz &&
			    blob_sz > 40)
			{
				size_t pre_off = 36, post_off = 36;
				size_t atom_idx = 0;
				size_t blob_end = blob_sz - 4;  // exclude CRC trailer
				int any_atom_mutated = 0;
				while (pre_off + 8 <= blob_end && post_off + 8 <= blob_end)
				{
					uint16_t pre_type  = ((uint16_t)pre_drain_blob[pre_off] << 8)
					                   | pre_drain_blob[pre_off + 1];
					uint16_t post_type = ((uint16_t)blob[post_off] << 8)
					                   | blob[post_off + 1];
					uint32_t pre_len = ((uint32_t)pre_drain_blob[pre_off+4] << 24)
					                 | ((uint32_t)pre_drain_blob[pre_off+5] << 16)
					                 | ((uint32_t)pre_drain_blob[pre_off+6] <<  8)
					                 | (uint32_t)pre_drain_blob[pre_off+7];
					uint32_t post_len = ((uint32_t)blob[post_off+4] << 24)
					                  | ((uint32_t)blob[post_off+5] << 16)
					                  | ((uint32_t)blob[post_off+6] <<  8)
					                  | (uint32_t)blob[post_off+7];
					uint32_t pre_pad  = (4 - (pre_len & 3)) & 3;
					uint32_t post_pad = (4 - (post_len & 3)) & 3;

					if (pre_type != post_type || pre_len != post_len)
					{
						vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
							"phase23_walk: atom_idx=%uz STRUCTURAL DIFF "
							"pre(type=0x%04xD len=%uD) post(type=0x%04xD len=%uD)",
							atom_idx, pre_type, pre_len, post_type, post_len);
						break;
					}

					// Same atom structurally. Diff the payload.
					int payload_diff = 0;
					uint32_t first_payload_diff = pre_len;
					uint32_t diff_bytes = 0;
					for (uint32_t i = 0; i < pre_len; i++)
					{
						if (pre_drain_blob[pre_off + 8 + i] !=
						    blob[post_off + 8 + i])
						{
							payload_diff = 1;
							if (first_payload_diff == pre_len) first_payload_diff = i;
							diff_bytes++;
						}
					}
					if (payload_diff)
					{
						any_atom_mutated = 1;
						vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
							"phase23_walk: atom_idx=%uz type=0x%04xD len=%uD "
							"MUTATED %uD bytes (first_diff_at_offset=%uD)",
							atom_idx, pre_type, pre_len,
							diff_bytes, first_payload_diff);
					}

					pre_off  += 8 + pre_len  + pre_pad;
					post_off += 8 + post_len + post_pad;
					atom_idx++;
				}
				if (!any_atom_mutated)
				{
					vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
						"phase23_walk: %uz atoms — ALL IDENTICAL pre/post drain "
						"(only CRC at blob tail changes; no content mutation)",
						atom_idx);
				}
			}
			else if (pre_drain_blob != NULL && pre_drain_sz != blob_sz)
			{
				vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
					"phase23_walk: blob size changed pre=%uz post=%uz",
					pre_drain_sz, blob_sz);
			}
			if (pre_drain_blob != NULL) av_free(pre_drain_blob);
			pre_drain_blob = NULL;

			*state->state_out_data = blob;
			*state->state_out_size = blob_sz;
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"audio_encoder_flush: captured encoder state (%uz bytes)",
				blob_sz);
		}
		else
		{
			*state->state_out_data = NULL;
			*state->state_out_size = 0;
			vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
				"audio_encoder_flush: avcodec_get_encoder_state failed %d", grc);
		}
#else
		*state->state_out_data = NULL;
		*state->state_out_size = 0;
#endif
	}

#if VOD_HAVE_ENCODER_STATE_API
	if (pre_drain_blob != NULL) av_free(pre_drain_blob);
#endif

	return VOD_OK;
}

vod_status_t
audio_encoder_update_media_info(
	void* context,
	media_info_t* media_info)
{
	audio_encoder_state_t* state = context;
	AVCodecContext *encoder = state->encoder;
	u_char* new_extra_data;

	if (encoder->time_base.num != 1)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_encoder_update_media_info: unexpected encoder time base %d/%d",
			encoder->time_base.num, encoder->time_base.den);
		return VOD_UNEXPECTED;
	}

	media_info->timescale = encoder->time_base.den;
	media_info->bitrate = encoder->bit_rate;

	media_info->u.audio.object_type_id = 0x40;		// ffmpeg always writes 0x40 (ff_mp4_obj_type)

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 23, 100)
	media_info->u.audio.channels = encoder->ch_layout.nb_channels;
	media_info->u.audio.channel_layout = encoder->ch_layout.u.mask;
#else
	media_info->u.audio.channels = encoder->channels;
	media_info->u.audio.channel_layout = encoder->channel_layout;
#endif

	media_info->u.audio.bits_per_sample = AUDIO_ENCODER_BITS_PER_SAMPLE;
	media_info->u.audio.packet_size = 0;			// ffmpeg always writes 0 (mov_write_audio_tag)
	media_info->u.audio.sample_rate = encoder->sample_rate;

	// Phase 7: AAC encoder's initial_padding is the MDCT lookahead — 1024
	// samples for the native AAC encoder at line ~1192 in libavcodec/aacenc.c.
	// Expose it as codec_delay (nanoseconds, consistent with existing Opus
	// path in mp4_parser.c:2210) so mp4_init_segment can emit an edts/elst
	// edit list that tells the player to skip these priming samples.
	if (encoder->initial_padding > 0 && encoder->sample_rate > 0)
	{
		media_info->codec_delay = (uint64_t)encoder->initial_padding
			* 1000000000 / encoder->sample_rate;
	}
	else
	{
		media_info->codec_delay = 0;
	}

	new_extra_data = vod_alloc(state->request_context->pool, encoder->extradata_size);
	if (new_extra_data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"audio_encoder_update_media_info: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	vod_memcpy(new_extra_data, encoder->extradata, encoder->extradata_size);

	media_info->extra_data.data = new_extra_data;
	media_info->extra_data.len = encoder->extradata_size;

	return VOD_OK;
}
