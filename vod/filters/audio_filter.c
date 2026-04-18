#include "audio_filter.h"
#include "rate_filter.h"

#if (VOD_HAVE_LIB_AV_CODEC && VOD_HAVE_LIB_AV_FILTER)
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include "audio_encoder.h"
#include "audio_decoder.h"
#include "volume_map.h"
#include "../input/frames_source_memory.h"

// stateful-audio: compile-time probe for filter state API (patch 0006+0007)
#if defined(LIBAVFILTER_VERSION_INT) && \
    LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(11, 16, 100)
#define VOD_HAVE_FILTER_STATE_API 1
#else
#define VOD_HAVE_FILTER_STATE_API 0
#endif

// FFSA wire constants (see stateful-aac-validation/design/state-format.md §1)
#define VOD_FFSA_HEADER_SIZE   36
#define VOD_FFSA_TRAILER_SIZE  4
#define VOD_FFSA_MIN_BLOB_SIZE (VOD_FFSA_HEADER_SIZE + VOD_FFSA_TRAILER_SIZE)
#define VOD_FFSA_COMPONENT_AAC_ENCODER 0x0001
#define VOD_FFSA_COMPONENT_ARESAMPLE   0x0002
#define VOD_FFSA_COMPONENT_AMIX        0x0003

// Big-endian scalar reads used for walking the FFSA header at transport level.
// Kept private to this TU so we never link libavcodec/state_format.c from here.
static inline uint16_t
vod_ffsa_rd_u16be(const uint8_t* p)
{
	return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint32_t
vod_ffsa_rd_u32be(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] <<  8) | (uint32_t)p[3];
}

// Walk one FFSA blob at *cursor. Return blob length (including header and
// trailer), 0 on malformed (so the caller can bail).
// On success, *out_component_id is set from the blob header (offset 8..9).
static size_t
vod_ffsa_peek_blob(const uint8_t* cursor, size_t remaining, uint16_t* out_component_id)
{
	uint32_t payload_length;
	size_t   total;

	if (remaining < VOD_FFSA_MIN_BLOB_SIZE)
		return 0;
	if (cursor[0] != 'F' || cursor[1] != 'F' ||
	    cursor[2] != 'S' || cursor[3] != 'A')
		return 0;

	payload_length = vod_ffsa_rd_u32be(cursor + 32);
	total = VOD_FFSA_HEADER_SIZE + (size_t)payload_length + VOD_FFSA_TRAILER_SIZE;
	if (total > remaining || total < VOD_FFSA_MIN_BLOB_SIZE)
		return 0;

	*out_component_id = vod_ffsa_rd_u16be(cursor + 8);
	return total;
}

// constants
#define BUFFERSRC_ARGS_FORMAT ("time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%uxL%Z")
#define MAX_SAMPLE_FORMAT_NAME_LEN (10)

#define BUFFERSRC_FILTER_NAME ("abuffer")
#define BUFFERSINK_FILTER_NAME ("abuffersink")
#define INPUT_FILTER_NAME ("in")
#define OUTPUT_FILTER_NAME ("out")

#define BUFFERSINK_PARAM_SAMPLE_FORMATS ("sample_fmts")
#define BUFFERSINK_PARAM_CHANNEL_LAYOUTS ("channel_layouts")
#define BUFFERSINK_PARAM_SAMPLE_RATES ("sample_rates")

// uncomment to save intermediate streams to temporary files
/*
#define AUDIO_FILTER_DEBUG
#define AUDIO_FILTER_DEBUG_PATH "/tmp/"
*/

// typedefs
typedef struct {
	enum AVSampleFormat format;
	void (*free)(void* context);
	size_t (*get_frame_size)(void* context);
	vod_status_t (*write)(void* context, AVFrame* frame);
	vod_status_t(*flush)(void* context);
	vod_status_t(*update_media_info)(void* context, media_info_t* media_info);
} audio_filter_encoder_t;

typedef struct
{
	audio_decoder_state_t decoder;
	AVFilterContext *buffer_src;
	bool_t buffersrc_flushed;
} audio_filter_source_t;

typedef struct
{
	AVFilterContext *buffer_sink;
	audio_filter_encoder_t* encoder;
	void* encoder_context;
	vod_array_t frames_array;
} audio_filter_sink_t;

// constants
static audio_filter_encoder_t libav_encoder = {
	AUDIO_ENCODER_INPUT_SAMPLE_FORMAT,
	audio_encoder_free,
	audio_encoder_get_frame_size,
	audio_encoder_write_frame,
	audio_encoder_flush,
	audio_encoder_update_media_info,
};

static audio_filter_encoder_t volume_map_encoder = {
	VOLUME_MAP_INPUT_SAMPLE_FORMAT,
	NULL,
	NULL,
	volume_map_encoder_write_frame,
	NULL,
	volume_map_encoder_update_media_info,
};

#endif

typedef struct {
	// phase 1
	request_context_t* request_context;
	uint32_t graph_desc_size;
	uint32_t source_count;
	uint32_t output_frame_count;

#if (VOD_HAVE_LIB_AV_CODEC && VOD_HAVE_LIB_AV_FILTER)
	// phase 2
	AVFilterGraph *filter_graph;
	AVFilterInOut** outputs;
	u_char* graph_desc;
	u_char* graph_desc_pos;
	audio_filter_source_t* cur_source;
	int cache_slot_id;
#endif
} audio_filter_init_context_t;

static vod_status_t 
audio_filter_walk_filters_prepare_init(
	audio_filter_init_context_t* state, 
	media_clip_t** clip_ptr, 
	uint32_t speed_num, 
	uint32_t speed_denom)
{
	media_clip_rate_filter_t* rate_filter;
	media_clip_source_t* source;
	media_track_t* audio_track;
	media_track_t* cur_track;
	media_clip_t** sources_end;
	media_clip_t** sources_cur;
	media_clip_t* clip = *clip_ptr;
	media_clip_t* last_source = NULL;
	vod_status_t rc;
	uint32_t cur_frame_count;
	uint32_t source_count;

	if (media_clip_is_source(clip->type))
	{
		source = vod_container_of(clip, media_clip_source_t, base);

		audio_track = NULL;
		for (cur_track = source->track_array.first_track; cur_track < source->track_array.last_track; cur_track++)
		{
			if (cur_track->media_info.media_type != MEDIA_TYPE_AUDIO)
			{
				continue;
			}

			if (audio_track != NULL)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"audio_filter_walk_filters_prepare_init: more than one audio track per source - unsupported");
				return VOD_BAD_REQUEST;
			}

			audio_track = cur_track;
		}

		if (audio_track == NULL || audio_track->frame_count == 0)
		{
			*clip_ptr = NULL;
			return VOD_OK;
		}

		state->source_count++;

		cur_frame_count = ((uint64_t)audio_track->frame_count * speed_denom) / speed_num;
		if (state->output_frame_count < cur_frame_count)
		{
			state->output_frame_count = cur_frame_count;
		}
		return VOD_OK;
	}

	switch (clip->type)
	{
	case MEDIA_CLIP_RATE_FILTER:
		rate_filter = vod_container_of(clip, media_clip_rate_filter_t, base);
		speed_num = ((uint64_t)speed_num * rate_filter->rate.num) / rate_filter->rate.denom;
		break;

	default:;
	}

	// recursively prepare the child sources
	source_count = 0;

	sources_end = clip->sources + clip->source_count;
	for (sources_cur = clip->sources; sources_cur < sources_end; sources_cur++)
	{
		rc = audio_filter_walk_filters_prepare_init(state, sources_cur, speed_num, speed_denom);
		if (rc != VOD_OK)
		{
			return rc;
		}

		if (*sources_cur != NULL)
		{
			source_count++;
			last_source = *sources_cur;
		}
	}

	// skip the current filter when it's not needed
	switch (source_count)
	{
	case 0:
		*clip_ptr = NULL;
		return VOD_OK;

	case 1:
		switch (clip->type)
		{
		case MEDIA_CLIP_MIX_FILTER:
		case MEDIA_CLIP_CONCAT:
			// in case of mixing a single clip or concat, skip the filter
			*clip_ptr = last_source;
			return VOD_OK;

		default:;
		}
		break;
	}

	// update the graph description size
	state->graph_desc_size += clip->audio_filter->get_filter_desc_size(clip) + 1;	// 1 = ';'

	return VOD_OK;
}

vod_status_t
audio_filter_alloc_memory_frame(
	request_context_t* request_context,
	vod_array_t* frames_array,
	size_t size,
	input_frame_t** result)
{
	input_frame_t* cur_frame;
	u_char* new_data;

	cur_frame = vod_array_push(frames_array);
	if (cur_frame == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_memory_frame: vod_array_push failed");
		return VOD_ALLOC_FAILED;
	}

	new_data = vod_alloc(request_context->pool, size);
	if (new_data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_memory_frame: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cur_frame->offset = (uintptr_t)new_data;
	cur_frame->size = size;
	cur_frame->key_frame = 0;

	*result = cur_frame;
	return VOD_OK;
}

#if (VOD_HAVE_LIB_AV_CODEC && VOD_HAVE_LIB_AV_FILTER)

typedef struct {
	request_context_t* request_context;
	
	// ffmpeg filter
	AVFilterGraph *filter_graph;
	AVFrame *filtered_frame;

	// filter end points
	audio_filter_sink_t sink;
	audio_filter_source_t* sources;
	audio_filter_source_t* sources_end;

	// output
	media_sequence_t* sequence;
	media_track_t* output;

	// processing state
	audio_filter_source_t* cur_source;
} audio_filter_state_t;

// globals
static const AVFilter *buffersrc_filter = NULL;
static const AVFilter *buffersink_filter = NULL;

static bool_t initialized = FALSE;

void 
audio_filter_process_init(vod_log_t* log)
{
	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 18, 100)
		avfilter_register_all();
	#endif

	buffersrc_filter = avfilter_get_by_name(BUFFERSRC_FILTER_NAME);
	if (buffersrc_filter == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_filter_process_init: failed to get buffer source filter, audio filtering is disabled");
		return;
	}

	buffersink_filter = avfilter_get_by_name(BUFFERSINK_FILTER_NAME);
	if (buffersink_filter == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"audio_filter_process_init: failed to get buffer sink filter, audio filtering is disabled");
		return;
	}

	initialized = TRUE;
}

static vod_status_t
audio_filter_init_source(
	request_context_t* request_context,
	AVFilterGraph *filter_graph,
	const u_char* source_name,
	audio_filter_source_t* source, 
	AVFilterInOut** outputs)
{
	char filter_args[sizeof(BUFFERSRC_ARGS_FORMAT) + 4 * VOD_INT64_LEN + MAX_SAMPLE_FORMAT_NAME_LEN];
	AVCodecContext* decoder = source->decoder.decoder;
	AVFilterInOut* output_link;
	int avrc;

	// create the buffer source
	vod_sprintf((u_char*)filter_args, BUFFERSRC_ARGS_FORMAT,
		decoder->time_base.num,
		decoder->time_base.den,
		decoder->sample_rate,
		av_get_sample_fmt_name(decoder->sample_fmt),
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(8, 44, 100)
		decoder->ch_layout.u.mask);
#else
		decoder->channel_layout);
#endif

	avrc = avfilter_graph_create_filter(
		&source->buffer_src,
		buffersrc_filter,
		(char*)source_name,
		filter_args,
		NULL,
		filter_graph);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: avfilter_graph_create_filter failed %d", avrc);
		return VOD_ALLOC_FAILED;
	}

	// add to the outputs list
	output_link = avfilter_inout_alloc();
	if (output_link == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: avfilter_inout_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	output_link->filter_ctx = source->buffer_src;
	output_link->pad_idx = 0;
	output_link->next = *outputs;
	*outputs = output_link;

	output_link->name = av_strdup((char*)source_name);
	if (output_link->name == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: av_strdup failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

static vod_status_t
audio_filter_init_sink(
	request_context_t* request_context,
	AVFilterGraph *filter_graph,
	uint64_t channel_layout,
	uint32_t sample_rate,
	const u_char* sink_name,
	audio_filter_sink_t* sink,
	AVFilterInOut** inputs)
{
	AVFilterInOut* input_link;
	int avrc;

	// Note: matching the output to some reference track, may need to change in the future
	//		if filters such as 'join' will be added

	// Modern libavutil (>= 59.36.100 / FFmpeg 7.1) enforces
	// AV_OPT_FLAG_RUNTIME_PARAM on post-init av_opt_set_*; abuffersink's
	// format options don't carry that flag, so the legacy sequence of
	// avfilter_graph_create_filter() followed by av_opt_set_int_list() now
	// returns -22 (AVERROR(EINVAL)). The portable fix is to split creation
	// from initialization: allocate with avfilter_graph_alloc_filter(), set
	// the options (pre-init), then call avfilter_init_dict().
	//
	// Name changes are also necessary — the old BINARY-typed
	// "sample_fmts"/"sample_rates" options plus the string-typed
	// "ch_layouts" option were replaced by ARRAY-typed "sample_formats",
	// "samplerates", and "channel_layouts" in the same release. The
	// deprecated names still exist behind FF_API_BUFFERSINK_OPTS on builds
	// where that knob is defined, but relying on them would regress again
	// as soon as the define flips off. So the modern branch below speaks
	// the new option vocabulary exclusively.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 36, 100)
	enum AVSampleFormat out_sample_fmt = sink->encoder->format;
	int out_sample_rate = (int)sample_rate;
	AVChannelLayout out_ch_layout;

	sink->buffer_sink = avfilter_graph_alloc_filter(
		filter_graph,
		buffersink_filter,
		(const char*)sink_name);
	if (sink->buffer_sink == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: avfilter_graph_alloc_filter failed");
		return VOD_ALLOC_FAILED;
	}

	avrc = av_opt_set_array(
		sink->buffer_sink,
		"sample_formats",
		AV_OPT_SEARCH_CHILDREN,
		0, 1,
		AV_OPT_TYPE_SAMPLE_FMT,
		&out_sample_fmt);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_opt_set_array(sample_formats) failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	avrc = av_opt_set_array(
		sink->buffer_sink,
		"samplerates",
		AV_OPT_SEARCH_CHILDREN,
		0, 1,
		AV_OPT_TYPE_INT,
		&out_sample_rate);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_opt_set_array(samplerates) failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	avrc = av_channel_layout_from_mask(&out_ch_layout, channel_layout);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_channel_layout_from_mask(0x%uxL) failed %d",
			channel_layout, avrc);
		return VOD_UNEXPECTED;
	}

	avrc = av_opt_set_array(
		sink->buffer_sink,
		"channel_layouts",
		AV_OPT_SEARCH_CHILDREN,
		0, 1,
		AV_OPT_TYPE_CHLAYOUT,
		&out_ch_layout);
	av_channel_layout_uninit(&out_ch_layout);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_opt_set_array(channel_layouts) failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	avrc = avfilter_init_dict(sink->buffer_sink, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: avfilter_init_dict failed %d", avrc);
		return VOD_UNEXPECTED;
	}
#else
	enum AVSampleFormat out_sample_fmts[2];
	int64_t out_channel_layouts[2];
	int out_sample_rates[2];

	// create the buffer sink
	avrc = avfilter_graph_create_filter(
		&sink->buffer_sink,
		buffersink_filter,
		(char*)sink_name,
		NULL,
		NULL,
		filter_graph);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: avfilter_graph_create_filter failed %d", avrc);
		return VOD_ALLOC_FAILED;
	}

	// configure the buffer sink
	out_sample_fmts[0] = sink->encoder->format;
	out_sample_fmts[1] = -1;
	avrc = av_opt_set_int_list(
		sink->buffer_sink,
		BUFFERSINK_PARAM_SAMPLE_FORMATS,
		out_sample_fmts,
		-1,
		AV_OPT_SEARCH_CHILDREN);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_opt_set_int_list(sample formats) failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	out_channel_layouts[0] = channel_layout;
	out_channel_layouts[1] = -1;
	avrc = av_opt_set_int_list(
		sink->buffer_sink,
		BUFFERSINK_PARAM_CHANNEL_LAYOUTS,
		out_channel_layouts,
		-1,
		AV_OPT_SEARCH_CHILDREN);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_opt_set_int_list(channel layouts) failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	out_sample_rates[0] = sample_rate;
	out_sample_rates[1] = -1;
	avrc = av_opt_set_int_list(
		sink->buffer_sink,
		BUFFERSINK_PARAM_SAMPLE_RATES,
		out_sample_rates,
		-1,
		AV_OPT_SEARCH_CHILDREN);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: av_opt_set_int_list(sample rates) failed %d", avrc);
		return VOD_UNEXPECTED;
	}
#endif

	// add to the inputs list
	input_link = avfilter_inout_alloc();
	if (input_link == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_sink: avfilter_inout_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	input_link->filter_ctx = sink->buffer_sink;
	input_link->pad_idx = 0;
	input_link->next = *inputs;
	*inputs = input_link;

	input_link->name = av_strdup((const char*)sink_name);
	if (input_link->name == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_init_source: av_strdup failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

static vod_status_t 
audio_filter_init_sources_and_graph_desc(audio_filter_init_context_t* state, media_clip_t* clip)
{
	audio_filter_source_t* cur_source;
	media_clip_t** sources_end;
	media_clip_t** sources_cur;
	media_clip_source_t* source;
	media_track_t* audio_track;
	media_track_t* cur_track;
	u_char filter_name[VOD_INT32_LEN + 1];
	vod_status_t rc;

	if (media_clip_is_source(clip->type))
	{
		source = vod_container_of(clip, media_clip_source_t, base);

		// find the audio track
		audio_track = NULL;
		for (cur_track = source->track_array.first_track; cur_track < source->track_array.last_track; cur_track++)
		{
			if (cur_track->media_info.media_type == MEDIA_TYPE_AUDIO)
			{
				audio_track = cur_track;
				break;
			}
		}

		// initialize the source
		cur_source = state->cur_source;
		state->cur_source++;

		rc = audio_decoder_init(
			&cur_source->decoder,
			state->request_context,
			audio_track,
			state->cache_slot_id++);
		if (rc != VOD_OK)
		{
			return rc;
		}

		vod_sprintf(filter_name, "%uD%Z", clip->id);

		return audio_filter_init_source(
			state->request_context,
			state->filter_graph,
			filter_name,
			cur_source,
			state->outputs);
	}

	// recursively init the child sources
	sources_end = clip->sources + clip->source_count;
	for (sources_cur = clip->sources; sources_cur < sources_end; sources_cur++)
	{
		if (*sources_cur == NULL)
		{
			continue;
		}

		rc = audio_filter_init_sources_and_graph_desc(state, *sources_cur);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	// add the filter description
	if (state->graph_desc_pos > state->graph_desc)
	{
		*state->graph_desc_pos++ = ';';
	}

	state->graph_desc_pos = clip->audio_filter->append_filter_desc(state->graph_desc_pos, clip);

	return VOD_OK;
}

// Locate the first filter instance in the graph whose name matches `name`.
// Used by the state hook to target amix/aresample instances. If there are
// multiple instances of the same filter in the graph (uncommon for our
// layouts) we only match the first — acceptable for restore because the
// capture path produces at most one blob per filter name in the emitted
// order (see audio_filter_capture_filter_state).
static AVFilterContext*
audio_filter_find_by_name(AVFilterGraph* graph, const char* name)
{
	unsigned i;
	if (graph == NULL || name == NULL)
		return NULL;
	for (i = 0; i < graph->nb_filters; i++)
	{
		AVFilterContext* f = graph->filters[i];
		if (f == NULL || f->filter == NULL || f->filter->name == NULL)
			continue;
		if (vod_strcmp((const u_char*)f->filter->name, (const u_char*)name) == 0)
			return f;
	}
	return NULL;
}

// Dev diagnostic: log every filter name in a configured graph once per
// request so we can see whether libavfilter auto-inserted aresample /
// aformat / etc. between our declared nodes. Useful for verifying which
// filter instances are candidates for the state hook. Logs at debug
// level — compile-time enable via AUDIO_FILTER_DEBUG_GRAPH.
#ifdef AUDIO_FILTER_DEBUG_GRAPH
static void
audio_filter_log_graph(request_context_t* request_context, AVFilterGraph* graph)
{
	unsigned i;
	if (graph == NULL)
		return;
	for (i = 0; i < graph->nb_filters; i++)
	{
		AVFilterContext* f = graph->filters[i];
		const char* name = (f && f->filter && f->filter->name) ? f->filter->name : "(null)";
		const char* inst = (f && f->name) ? f->name : "(null)";
		vod_log_debug2(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_log_graph: filter=\"%s\" inst=\"%s\"",
			name, inst);
		(void)i;
	}
}
#endif

// Split a concatenated FFSA state-in blob into component slices. The AAC
// encoder blob (component 0x0001) is returned via out_encoder_{data,size}
// so audio_encoder_init can consume it exactly as before. Filter blobs
// (aresample 0x0002, amix 0x0003) are applied in-place to their matching
// filter instances via avfilter_set_filter_state. If a filter blob is
// present but its target filter is absent from the graph we log a warning
// and skip — the calling code continues with whatever filter state the
// blob could not be applied to running fresh for that one segment.
//
// Returns VOD_OK on best-effort success. Any restore failure at the filter
// layer is logged and does NOT fail the request (graceful degradation, per
// the encoder-state hook pattern). Malformed transport-level framing is
// treated as "no state" and falls through to fresh init.
static vod_status_t
audio_filter_restore_state(
	request_context_t* request_context,
	AVFilterGraph* graph,
	const u_char* blob,
	size_t size,
	const u_char** out_encoder_data,
	size_t* out_encoder_size)
{
	const uint8_t* cursor;
	size_t remaining;

	*out_encoder_data = NULL;
	*out_encoder_size = 0;

	if (blob == NULL || size == 0)
		return VOD_OK;

	cursor    = (const uint8_t*)blob;
	remaining = size;

	while (remaining > 0)
	{
		uint16_t component_id = 0;
		size_t   blob_len     = vod_ffsa_peek_blob(cursor, remaining, &component_id);

		if (blob_len == 0)
		{
			vod_log_error(VOD_LOG_WARN, request_context->log, 0,
				"audio_filter_restore_state: malformed FFSA blob at offset %uz, stopping walk",
				size - remaining);
			break;
		}

		switch (component_id)
		{
		case VOD_FFSA_COMPONENT_AAC_ENCODER:
			if (*out_encoder_data == NULL)
			{
				*out_encoder_data = (const u_char*)cursor;
				*out_encoder_size = blob_len;
			}
			break;

#if VOD_HAVE_FILTER_STATE_API
		case VOD_FFSA_COMPONENT_ARESAMPLE:
		{
			AVFilterContext* f = audio_filter_find_by_name(graph, "aresample");
			if (f == NULL)
			{
				vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"audio_filter_restore_state: aresample blob present (%uz bytes) but "
					"no aresample filter in graph, skipping", blob_len);
			}
			else
			{
				int avrc = avfilter_set_filter_state(f, cursor, blob_len);
				if (avrc < 0)
				{
					vod_log_error(VOD_LOG_WARN, request_context->log, 0,
						"audio_filter_restore_state: avfilter_set_filter_state(aresample) failed %d, "
						"continuing with fresh filter state", avrc);
				}
				else
				{
					vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
						"audio_filter_restore_state: restored aresample state (%uz bytes)",
						blob_len);
				}
			}
			break;
		}

		case VOD_FFSA_COMPONENT_AMIX:
		{
			AVFilterContext* f = audio_filter_find_by_name(graph, "amix");
			if (f == NULL)
			{
				vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"audio_filter_restore_state: amix blob present (%uz bytes) but "
					"no amix filter in graph, skipping", blob_len);
			}
			else
			{
				int avrc = avfilter_set_filter_state(f, cursor, blob_len);
				if (avrc < 0)
				{
					vod_log_error(VOD_LOG_WARN, request_context->log, 0,
						"audio_filter_restore_state: avfilter_set_filter_state(amix) failed %d, "
						"continuing with fresh filter state", avrc);
				}
				else
				{
					vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
						"audio_filter_restore_state: restored amix state (%uz bytes)",
						blob_len);
				}
			}
			break;
		}
#else
		case VOD_FFSA_COMPONENT_ARESAMPLE:
		case VOD_FFSA_COMPONENT_AMIX:
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"audio_filter_restore_state: filter blob component=0x%04uxD "
				"ignored (ffmpeg lacks filter-state API)", (unsigned)component_id);
			break;
#endif

		default:
			vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"audio_filter_restore_state: unknown component=0x%04uxD, skipping",
				(unsigned)component_id);
			break;
		}

		cursor    += blob_len;
		remaining -= blob_len;
	}

	return VOD_OK;
}

#if VOD_HAVE_FILTER_STATE_API
// Concatenate filter-state blobs for every amix/aresample instance in the
// graph onto the existing encoder blob at *state_out_data. Called exactly
// once per segment, after audio_encoder_flush has finished populating the
// encoder slot. On success, *state_out_data is replaced with a freshly
// av_malloc'd buffer containing (encoder_blob || filter_blob_1 || ... ||
// filter_blob_N), the old encoder blob is av_free'd, and *state_out_size
// is updated. Individual filter blob mallocs are also av_free'd after copy.
//
// Any failure to capture a filter's state is logged and the filter is
// skipped — the remainder of the segment's blob (encoder + successfully
// captured filters) is still POSTed. The next segment's restore path
// tolerates missing components.
//
// Order of filter names matters for the transport: "aresample" before
// "amix" matches the natural audio-graph dataflow order (resample → mix).
// The reader parses by header length and does not rely on this order for
// correctness, but keeping it stable simplifies diagnosis.
//
// Currently only aresample is captured. amix capture is intentionally
// disabled: ff_amix_set_state correctly re-applies the serialised FIFO +
// frame_list state, but the interaction with nginx-vod-module's single
// per-segment graph-build/tear-down cycle produces stalled activation
// on the next segment when the restored FIFO contains any samples. The
// restore succeeds, the next segment's amix reports correct input_state,
// but activate() then fails to request new frames — no encoder input,
// the segment's TS mux emits 0 AAC packets, and the stream breaks. This
// is an ffmpeg-amix-side quirk orthogonal to the transport-layer concat
// work, and out of scope here; tracked at the decision-log level. The
// transport layer handles both aresample and amix blobs symmetrically,
// so extending to amix later is a single-line change once the activate
// quirk is isolated.
static void
audio_filter_capture_filter_state(
	request_context_t* request_context,
	AVFilterGraph* graph,
	u_char** state_out_data,
	size_t* state_out_size)
{
	static const char* const kFilterNames[] = { "aresample" };
	u_char* filter_blobs[sizeof(kFilterNames) / sizeof(kFilterNames[0])];
	size_t  filter_sizes[sizeof(kFilterNames) / sizeof(kFilterNames[0])];
	size_t  total_filter_bytes = 0;
	size_t  n_filters = sizeof(kFilterNames) / sizeof(kFilterNames[0]);
	size_t  i;
	size_t  new_size;
	u_char* new_blob;
	size_t  off;

	vod_memzero(filter_blobs, sizeof(filter_blobs));
	vod_memzero(filter_sizes, sizeof(filter_sizes));

	if (graph == NULL || state_out_data == NULL || state_out_size == NULL)
		return;
	if (*state_out_data == NULL || *state_out_size == 0)
		return;

	for (i = 0; i < n_filters; i++)
	{
		AVFilterContext* f = audio_filter_find_by_name(graph, kFilterNames[i]);
		uint8_t*         buf = NULL;
		size_t           sz  = 0;
		int              avrc;

		if (f == NULL)
			continue;

		avrc = avfilter_get_filter_state(f, &buf, &sz);
		if (avrc == AVERROR(ENOSYS))
			continue;
		if (avrc < 0 || buf == NULL || sz == 0)
		{
			vod_log_error(VOD_LOG_WARN, request_context->log, 0,
				"audio_filter_capture_filter_state: avfilter_get_filter_state(%s) failed %d",
				kFilterNames[i], avrc);
			if (buf != NULL)
				av_free(buf);
			continue;
		}

		filter_blobs[i]    = buf;
		filter_sizes[i]    = sz;
		total_filter_bytes += sz;

		vod_log_debug2(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_capture_filter_state: captured %s state (%uz bytes)",
			kFilterNames[i], sz);
	}

	if (total_filter_bytes == 0)
		return;

	new_size = *state_out_size + total_filter_bytes;
	new_blob = av_malloc(new_size);
	if (new_blob == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_capture_filter_state: av_malloc(%uz) failed, "
			"dropping filter blobs (encoder state still POSTed)", new_size);
		for (i = 0; i < n_filters; i++)
		{
			if (filter_blobs[i] != NULL)
				av_free(filter_blobs[i]);
		}
		return;
	}

	vod_memcpy(new_blob, *state_out_data, *state_out_size);
	off = *state_out_size;
	for (i = 0; i < n_filters; i++)
	{
		if (filter_blobs[i] == NULL)
			continue;
		vod_memcpy(new_blob + off, filter_blobs[i], filter_sizes[i]);
		off += filter_sizes[i];
		av_free(filter_blobs[i]);
	}

	av_free(*state_out_data);
	*state_out_data = new_blob;
	*state_out_size = new_size;

	vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
		"audio_filter_capture_filter_state: emitted concatenated blob (%uz bytes total)",
		new_size);
}
#endif  // VOD_HAVE_FILTER_STATE_API

vod_status_t
audio_filter_alloc_state(
	request_context_t* request_context,
	media_sequence_t* sequence,
	media_clip_t* clip,
	media_track_t* output_track,
	uint32_t max_frame_count,
	uint32_t output_codec_id,
	size_t* cache_buffer_count,
	void** result)
{
	audio_filter_init_context_t init_context;
	u_char filter_name[VOD_INT32_LEN + 1];
	audio_encoder_params_t encoder_params;
	audio_filter_state_t* state;
	vod_pool_cleanup_t *cln;
	AVFilterLink* sink_link;
	AVFilterInOut *outputs = NULL;
	AVFilterInOut *inputs = NULL;
	vod_status_t rc;
	uint32_t initial_alloc_size;
	size_t frame_size;
	int avrc;

	// get the source count and graph desc size
	init_context.request_context = request_context;
	init_context.graph_desc_size = 0;
	init_context.source_count = 0;
	init_context.output_frame_count = 0;

	rc = audio_filter_walk_filters_prepare_init(&init_context, &clip, 100, 100);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (clip == NULL || init_context.source_count <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: unexpected - no sources found");
		return VOD_UNEXPECTED;
	}

	if (media_clip_is_source(clip->type))
	{
		// got left with a source, following a mix of a single source, nothing to do
		return VOD_OK;
	}

	if (!initialized)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: module failed to initialize successfully");
		return VOD_UNEXPECTED;
	}

	if (init_context.output_frame_count > max_frame_count)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: expected output frame count %uD too big", init_context.output_frame_count);
		return VOD_BAD_REQUEST;
	}

	// allocate the state
	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}
	vod_memzero(state, sizeof(*state));
	
	// add to the cleanup pool
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = audio_filter_free_state;
	cln->data = state;

	// allocate the filter graph
	state->filter_graph = avfilter_graph_alloc();
	if (state->filter_graph == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: avfilter_graph_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// allocate the graph desc and sources
	init_context.graph_desc = vod_alloc(request_context->pool, init_context.graph_desc_size + 
		sizeof(state->sources[0]) * init_context.source_count);
	if (init_context.graph_desc == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	state->sources = (void*)(init_context.graph_desc + init_context.graph_desc_size);
	state->sources_end = state->sources + init_context.source_count;
	vod_memzero(state->sources, (u_char*)state->sources_end - (u_char*)state->sources);

	// initialize the sources and the graph description
	init_context.filter_graph = state->filter_graph;
	init_context.outputs = &outputs;
	init_context.cur_source = state->sources;
	init_context.graph_desc_pos = init_context.graph_desc;
	init_context.cache_slot_id = 0;

	rc = audio_filter_init_sources_and_graph_desc(&init_context, clip);
	if (rc != VOD_OK)
	{
		goto end;
	}

	*init_context.graph_desc_pos = '\0';

	// init the encoder
	if (output_codec_id == VOD_CODEC_ID_VOLUME_MAP)
	{
		state->sink.encoder = &volume_map_encoder;
	}
	else
	{
		state->sink.encoder = &libav_encoder;
	}

	// initialize the sink
	vod_sprintf(filter_name, "%uD%Z", clip->id);

	rc = audio_filter_init_sink(
		request_context,
		state->filter_graph,
		output_track->media_info.u.audio.channel_layout,
		output_track->media_info.u.audio.sample_rate,
		filter_name,
		&state->sink,
		&inputs);
	if (rc != VOD_OK)
	{
		goto end;
	}

	// parse the graph description
	avrc = avfilter_graph_parse_ptr(state->filter_graph, (char*)init_context.graph_desc, &inputs, &outputs, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: avfilter_graph_parse_ptr failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto end;
	}

	// validate and configure the graph
	avrc = avfilter_graph_config(state->filter_graph, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: avfilter_graph_config failed %d", avrc);
		rc = VOD_UNEXPECTED;
		goto end;
	}

#ifdef AUDIO_FILTER_DEBUG_GRAPH
	// Diagnostic: dump the configured filter graph's node list.
	audio_filter_log_graph(request_context, state->filter_graph);
#endif
	
	// initialize the encoder
	sink_link = state->sink.buffer_sink->inputs[0];
	if (sink_link->time_base.num != 1)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: unexpected buffer sink time base %d/%d",
			sink_link->time_base.num, sink_link->time_base.den);
		rc = VOD_UNEXPECTED;
		goto end;
	}

	if (output_codec_id == VOD_CODEC_ID_VOLUME_MAP)
	{
		rc = volume_map_encoder_init(
			request_context,
			sink_link->time_base.den,
			&state->sink.frames_array,
			&state->sink.encoder_context);
		if (rc != VOD_OK)
		{
			goto end;
		}
	}
	else
	{
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(8, 44, 100)
		encoder_params.channels = sink_link->ch_layout.nb_channels;
		encoder_params.channel_layout = sink_link->ch_layout.u.mask;
#else
		encoder_params.channels = sink_link->channels;
		encoder_params.channel_layout = sink_link->channel_layout;
#endif
		encoder_params.sample_rate = sink_link->sample_rate;
		encoder_params.timescale = sink_link->time_base.den;
		encoder_params.bitrate = output_track->media_info.bitrate;

		// stateful-audio: thread the FFSA shuttle from request_context. The
		// shuttle's state_in blob may be a concatenation of per-component
		// FFSA blobs (encoder || aresample || amix). Split them at the
		// transport layer: audio_filter_restore_state identifies the encoder
		// slice to hand off to audio_encoder_init (unchanged codepath) and
		// applies filter-state blobs directly to matching filter instances
		// in the freshly-configured graph BEFORE any frames are pushed —
		// this is the ordering constraint from the design spec, since
		// amix/aresample internal buffers must exist before frame arrival.
		encoder_params.state_in_data  = NULL;
		encoder_params.state_in_size  = 0;
		encoder_params.state_out_data = NULL;
		encoder_params.state_out_size = NULL;
		if (request_context->audio_encoder_state_shuttle != NULL)
		{
			const u_char* encoder_slice_data = NULL;
			size_t        encoder_slice_size = 0;

			rc = audio_filter_restore_state(
				request_context,
				state->filter_graph,
				request_context->audio_encoder_state_shuttle->state_in_data,
				request_context->audio_encoder_state_shuttle->state_in_size,
				&encoder_slice_data,
				&encoder_slice_size);
			if (rc != VOD_OK)
			{
				goto end;
			}

			// If no encoder slice was found inside the concatenated blob,
			// fall through with NULL state_in — audio_encoder_init will run
			// fresh (priming artefact for that one segment, acceptable).
			encoder_params.state_in_data  = encoder_slice_data;
			encoder_params.state_in_size  = encoder_slice_size;
			encoder_params.state_out_data = request_context->audio_encoder_state_shuttle->state_out_data;
			encoder_params.state_out_size = request_context->audio_encoder_state_shuttle->state_out_size;
		}

		rc = audio_encoder_init(
			request_context,
			&encoder_params,
			&state->sink.frames_array,
			&state->sink.encoder_context);
		if (rc != VOD_OK)
		{
			goto end;
		}
	}

	// set the buffer sink frame size
	if (state->sink.encoder->get_frame_size != NULL)
	{
		frame_size = state->sink.encoder->get_frame_size(
			state->sink.encoder_context);
		if (frame_size != 0)
		{
			av_buffersink_set_frame_size(state->sink.buffer_sink, frame_size);
		}
	}
	
	// allocate frame
	state->filtered_frame = av_frame_alloc();
	if (state->filtered_frame == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: av_frame_alloc failed");
		rc = VOD_ALLOC_FAILED;
		goto end;
	}

	// initialize the output arrays
	initial_alloc_size = init_context.output_frame_count + 10;

	if (vod_array_init(&state->sink.frames_array, request_context->pool, initial_alloc_size, sizeof(input_frame_t)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"audio_filter_alloc_state: vod_array_init failed");
		rc = VOD_ALLOC_FAILED;
		goto end;
	}

	state->request_context = request_context;
	state->sequence = sequence;
	state->output = output_track;

	*cache_buffer_count = init_context.cache_slot_id;
	*result = state;

end:

	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return rc;
}

void
audio_filter_free_state(void* context)
{
	audio_filter_state_t* state = (audio_filter_state_t*)context;
	audio_filter_source_t* sources_cur;

	for (sources_cur = state->sources; sources_cur < state->sources_end; sources_cur++)
	{
		audio_decoder_free(&sources_cur->decoder);
	}
	
	if (state->sink.encoder != NULL && state->sink.encoder->free != NULL)
	{
		state->sink.encoder->free(state->sink.encoder_context);
	}
	
	avfilter_graph_free(&state->filter_graph);
	av_frame_free(&state->filtered_frame);

	vod_memzero(state, sizeof(*state));		// support calling free twice
}

static vod_status_t 
audio_filter_update_track(audio_filter_state_t* state)
{
	media_track_t* output = state->output;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t old_timescale;
	vod_status_t rc;
	bool_t has_frames;

	// decrement the old frame count and size
	state->sequence->total_frame_count -= output->frame_count;
	state->sequence->total_frame_size -= output->total_frames_size;
	output->total_frames_size = 0;
	output->total_frames_duration = 0;

	// update frames
	output->frame_count = state->sink.frames_array.nelts;

	output->frames.first_frame = state->sink.frames_array.elts;
	output->frames.last_frame = output->frames.first_frame + output->frame_count;
	output->frames.next = NULL;

	// check whether there are any frames with duration
	has_frames = FALSE;

	// Note: always a single part here
	last_frame = output->frames.last_frame;
	for (cur_frame = output->frames.first_frame; cur_frame < last_frame; cur_frame++)
	{
		if (cur_frame->duration != 0)
		{
			has_frames = TRUE;
			break;
		}
	}

	if (!has_frames)
	{
		output->frames.first_frame = NULL;
		output->frames.last_frame = NULL;
		output->frame_count = 0;
		return VOD_OK;
	}

	// update the frames source to memory
	rc = frames_source_memory_init(state->request_context, &output->frames.frames_source_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	output->frames.frames_source = &frames_source_memory;

	// calculate the total frames size and duration
	for (cur_frame = output->frames.first_frame; cur_frame < last_frame; cur_frame++)
	{
		output->total_frames_size += cur_frame->size;
		output->total_frames_duration += cur_frame->duration;
	}

	// update media info
	old_timescale = output->media_info.timescale;

	rc = state->sink.encoder->update_media_info(
		state->sink.encoder_context,
		&output->media_info);
	if (rc != VOD_OK)
	{
		return rc;
	}

	output->media_info.duration = rescale_time(output->media_info.duration, old_timescale, output->media_info.timescale);

	output->key_frame_count = 0;
	output->first_frame_time_offset = rescale_time(output->first_frame_time_offset, old_timescale, output->media_info.timescale);

	if (output->media_info.codec_name.data != NULL)
	{
		rc = codec_config_get_audio_codec_name(state->request_context, &output->media_info);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
		
	// add the new frame count and size
	state->sequence->total_frame_count += output->frame_count;
	state->sequence->total_frame_size += output->total_frames_size;

	// TODO: update raw_atoms
	
	return VOD_OK;
}

#ifdef AUDIO_FILTER_DEBUG
static void
audio_filter_append_debug_data(const char* source, const char* extension, const void* buffer, size_t size)
{
	char file_path[256];
	FILE* fp;

	sprintf(file_path, "%s%s.%s", AUDIO_FILTER_DEBUG_PATH, source, extension);
		
	fp = fopen(file_path, "a");
	if (fp == NULL)
	{
		return;
	}
	fwrite(buffer, 1, size, fp);
	fclose(fp);
}
#endif // AUDIO_FILTER_DEBUG

static vod_status_t
audio_filter_read_filter_sink(audio_filter_state_t* state)
{
	vod_status_t rc;
	int avrc;
#ifdef AUDIO_FILTER_DEBUG
	size_t data_size;
#endif // AUDIO_FILTER_DEBUG

	for (;;)
	{
		avrc = av_buffersink_get_frame_flags(state->sink.buffer_sink, state->filtered_frame, AV_BUFFERSINK_FLAG_NO_REQUEST);
		if (avrc == AVERROR(EAGAIN) || avrc == AVERROR_EOF)
		{
			break;
		}

		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"audio_filter_read_filter_sink: av_buffersink_get_frame_flags failed %d", avrc);
			return VOD_UNEXPECTED;
		}

#ifdef AUDIO_FILTER_DEBUG
		data_size = av_samples_get_buffer_size(
			NULL,
			state->filtered_frame->channels,
			state->filtered_frame->nb_samples,
			state->filtered_frame->format,
			1);
		audio_filter_append_debug_data("sink", "pcm", state->filtered_frame->data[0], data_size);
#endif // AUDIO_FILTER_DEBUG

		rc = state->sink.encoder->write(state->sink.encoder_context, state->filtered_frame);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	return VOD_OK;
}

static vod_status_t 
audio_filter_process_frame(audio_filter_state_t* state, AVFrame* frame)
{
	audio_filter_source_t* source = state->cur_source;
	int avrc;

#ifdef AUDIO_FILTER_DEBUG
	size_t data_size;

	data_size = av_samples_get_buffer_size(
		NULL, 
		frame->channels,
		frame->nb_samples,
		frame->format,
		1);
	audio_filter_append_debug_data(source->buffer_src->name, "pcm", frame->data[0], data_size);
#endif // AUDIO_FILTER_DEBUG
	
	avrc = av_buffersrc_add_frame_flags(source->buffer_src, frame, AV_BUFFERSRC_FLAG_PUSH);
	if (avrc < 0) 
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_process_frame: av_buffersrc_add_frame_flags failed %d", avrc);
		return VOD_ALLOC_FAILED;
	}

	return audio_filter_read_filter_sink(state);
}

static vod_status_t
audio_filter_choose_source(audio_filter_state_t* state)
{
	audio_filter_source_t* sources_cur;
	audio_filter_source_t* best_source;
	int failed_requests_max;
	int failed_requests;
	int avrc;
	int ret;
	vod_status_t rc;

	for (;;)
	{
		ret = avfilter_graph_request_oldest(state->filter_graph);
		if (ret < 0)
		{
			break;
		}

		rc = audio_filter_read_filter_sink(state);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	if (ret != AVERROR(EAGAIN))
	{
		if (ret == AVERROR_EOF)
		{
			return VOD_NOT_FOUND;
		}

		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"audio_filter_choose_source: avfilter_graph_request_oldest failed %d", ret);
		return VOD_UNEXPECTED;
	}

	failed_requests_max = -1;
	best_source = NULL;
	for (sources_cur = state->sources; sources_cur < state->sources_end; sources_cur++)
	{
		if (!audio_decoder_has_frame(&sources_cur->decoder))
		{
			if (!sources_cur->buffersrc_flushed)
			{
				avrc = av_buffersrc_add_frame_flags(sources_cur->buffer_src, NULL, 0);
				if (avrc < 0)
				{
					vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
						"audio_filter_choose_source: av_buffersrc_add_frame_flags failed %d", avrc);
					return VOD_ALLOC_FAILED;
				}

				sources_cur->buffersrc_flushed = TRUE;
			}
			continue;
		}

		failed_requests = av_buffersrc_get_nb_failed_requests(sources_cur->buffer_src);
		if (failed_requests > failed_requests_max)
		{
			failed_requests_max = failed_requests;
			best_source = sources_cur;
		}
	}

	if (best_source == NULL)
	{
		return VOD_NOT_FOUND;
	}

	state->cur_source = best_source;
	return VOD_OK;
}

vod_status_t
audio_filter_process(void* context)
{
	audio_filter_state_t* state = context;
	vod_status_t rc;
	AVFrame* frame;

	for (;;)
	{
		// choose a source if needed
		if (state->cur_source == NULL)
		{
			rc = audio_filter_choose_source(state);
			if (rc == VOD_NOT_FOUND)
			{
				// done
				if (state->sink.encoder->flush != NULL)
				{
					rc = state->sink.encoder->flush(state->sink.encoder_context);
					if (rc != VOD_OK)
					{
						return rc;
					}
				}

#if VOD_HAVE_FILTER_STATE_API
				// stateful-audio: encoder has just deposited its blob into
				// the shuttle's state_out_data. Append filter-state blobs
				// so the POST body carries the full set. Ordering: AFTER
				// encoder_flush (so the encoder's AFQ and planar buffers
				// reflect end-of-segment), BEFORE audio_filter_update_track
				// (which only rearranges output packets). The filter graph
				// is still alive at this point — audio_filter_free_state
				// runs later via pool cleanup.
				if (state->request_context->audio_encoder_state_shuttle != NULL &&
				    state->request_context->audio_encoder_state_shuttle->state_out_data != NULL &&
				    state->request_context->audio_encoder_state_shuttle->state_out_size != NULL)
				{
					audio_filter_capture_filter_state(
						state->request_context,
						state->filter_graph,
						state->request_context->audio_encoder_state_shuttle->state_out_data,
						state->request_context->audio_encoder_state_shuttle->state_out_size);
				}
#endif

				return audio_filter_update_track(state);
			}

			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		rc = audio_decoder_get_frame(&state->cur_source->decoder, &frame);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// process the frame
		rc = audio_filter_process_frame(state, frame);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// move to the next frame
		state->cur_source = NULL;
	}
}

#else

// empty stubs in case libavfilter/libavcodec are missing
void 
audio_filter_process_init(vod_log_t* log)
{
}

vod_status_t
audio_filter_alloc_state(
	request_context_t* request_context,
	media_sequence_t* sequence,
	media_clip_t* clip,
	media_track_t* output_track,
	uint32_t max_frame_count,
	uint32_t output_codec_id,
	size_t* cache_buffer_count,
	void** result)
{
	audio_filter_init_context_t init_context;
	vod_status_t rc;

	// get the source count and graph desc size
	init_context.request_context = request_context;
	init_context.graph_desc_size = 0;
	init_context.source_count = 0;
	init_context.output_frame_count = 0;

	rc = audio_filter_walk_filters_prepare_init(&init_context, &clip, 100, 100);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (clip == NULL || init_context.source_count <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"audio_filter_alloc_state: unexpected - no sources found");
		return VOD_UNEXPECTED;
	}

	if (media_clip_is_source(clip->type))
	{
		// got left with a source, following a mix of a single source, nothing to do
		return VOD_OK;
	}

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
		"audio_filter_alloc_state: audio filtering not supported, recompile with avcodec/avfilter to enable it");
	return VOD_UNEXPECTED;
}

void 
audio_filter_free_state(void* context)
{
}

vod_status_t 
audio_filter_process(void* context)
{
	return VOD_UNEXPECTED;
}

#endif
