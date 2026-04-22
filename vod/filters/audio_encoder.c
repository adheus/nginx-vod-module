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
	int avrc;

	// Phase 24 fix: DO NOT drain at segment boundary when stateful-audio
	// hook is active. Drain (avcodec_send_frame NULL + receive_packet
	// loop) causes two problems proven by test-harness/bit_exact_test.c
	// Run D (chunked, state-transfer, drain-every-chunk):
	//
	//   1. Drain's internal copy_input_samples(s, NULL) shifts and zeros
	//      the encoder's planar_samples lookahead region.
	//   2. Drain emits the pending AFQ frame as a packet that seg-N+1's
	//      restored state transfer would have emitted anyway via MDCT
	//      overlap from its restored planar_samples buffer.
	//
	// Together these make the post-drain encoder state represent
	// "encoder finished" rather than "encoder mid-stream", which is
	// incompatible with the library's bit-exact Run C continuity
	// contract. Run C (no per-chunk drain) produces 1380 packets
	// matching continuous reference exactly. Run D (per-chunk drain)
	// produces 1394 packets diverging at every boundary — 1194/1380
	// differ in size. User listening maps this 14-extra-packet
	// divergence to the audible "vinyl blip" at every segment boundary.
	//
	// Skipping drain means seg-N emits N-1 packets instead of N, but
	// the N-th packet's content is carried forward in the captured
	// AFQ state and emitted as seg-N+1's first output packet — exactly
	// what a continuous encoder does at the same timeline position.
	// Total audio content is preserved bit-exactly.
	//
	// Trade-off: seg-N fragment is ~23 ms shorter than EXTINF declares.
	// If segmenter's `accurate` duration mode computes per-segment
	// EXTINF from actual emitted sample count, the manifest stays
	// consistent. If it doesn't, player may see slight drift — but
	// state-transfer continuity beats drift-free-but-blipping audio.
	(void)avrc;



	// Phase 24: snapshot encoder state directly — no drain.
	// The encoder's AFQ pending frame stays in state and will be emitted
	// as seg-N+1's first output packet when state is restored (via MDCT
	// overlap from restored planar_samples). This preserves bit-exact
	// continuity across segment boundaries (proven by bit_exact_test
	// Run C: state transfer without per-chunk drain = 1380/1380 packets
	// match continuous reference exactly).
	if (state->state_out_data != NULL && state->state_out_size != NULL)
	{
#if VOD_HAVE_ENCODER_STATE_API
		uint8_t* blob = NULL;
		size_t   blob_sz = 0;
		int      grc = avcodec_get_encoder_state(state->encoder, &blob, &blob_sz);
		if (grc == 0 && blob != NULL && blob_sz > 0)
		{
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
