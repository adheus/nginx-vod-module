#include "key_change_filter.h"
#include "audio_filter.h"
#include "../media_set_parser.h"

// macros
//   asetrate shifts pitch by changing the sample rate, aresample restores it,
//   atempo compensates the resulting speed change.
//   pitch_ratio = 2^(semitones/12), tempo_ratio = 1/pitch_ratio
#define KEY_CHANGE_FILTER_DESC_PATTERN \
	"[%uD]asetrate=%uD*%uD.%04uD,aresample=%uD,atempo=%uD.%04uD[%uD]"

// the filter desc is long due to two decimal numbers + sample rate references
#define KEY_CHANGE_FILTER_MAX_DESC_SIZE \
	(sizeof(KEY_CHANGE_FILTER_DESC_PATTERN) + VOD_INT32_LEN * 8)

#define KEY_CHANGE_MIN_SEMITONES (-12)
#define KEY_CHANGE_MAX_SEMITONES (12)

// enums
enum {
	KEY_CHANGE_FILTER_PARAM_SEMITONES,
	KEY_CHANGE_FILTER_PARAM_SOURCE,

	KEY_CHANGE_FILTER_PARAM_COUNT
};

// constants
static json_object_key_def_t key_change_filter_params[] = {
	{ vod_string("semitones"),	VOD_JSON_INT,		KEY_CHANGE_FILTER_PARAM_SEMITONES },
	{ vod_string("source"),		VOD_JSON_OBJECT,	KEY_CHANGE_FILTER_PARAM_SOURCE },
	{ vod_null_string, 0, 0 }
};

// precomputed pitch ratios: 2^(semitones/12) * 10000, indexed by semitones+12
//   index 0  = -12 semitones = 0.5000x
//   index 12 =   0 semitones = 1.0000x
//   index 24 = +12 semitones = 2.0000x
static const uint32_t pitch_ratio_table[25] = {
	 5000,	//  -12: 2^(-12/12) = 0.5000
	 5297,	//  -11: 2^(-11/12) = 0.5297
	 5612,	//  -10: 2^(-10/12) = 0.5612
	 5946,	//   -9: 2^(-9/12)  = 0.5946
	 6300,	//   -8: 2^(-8/12)  = 0.6300
	 6674,	//   -7: 2^(-7/12)  = 0.6674
	 7071,	//   -6: 2^(-6/12)  = 0.7071
	 7492,	//   -5: 2^(-5/12)  = 0.7492
	 7937,	//   -4: 2^(-4/12)  = 0.7937
	 8409,	//   -3: 2^(-3/12)  = 0.8409
	 8909,	//   -2: 2^(-2/12)  = 0.8909
	 9439,	//   -1: 2^(-1/12)  = 0.9439
	10000,	//    0: 2^(0/12)   = 1.0000
	10595,	//   +1: 2^(1/12)   = 1.0595
	11225,	//   +2: 2^(2/12)   = 1.1225
	11892,	//   +3: 2^(3/12)   = 1.1892
	12599,	//   +4: 2^(4/12)   = 1.2599
	13348,	//   +5: 2^(5/12)   = 1.3348
	14142,	//   +6: 2^(6/12)   = 1.4142
	14983,	//   +7: 2^(7/12)   = 1.4983
	15874,	//   +8: 2^(8/12)   = 1.5874
	16818,	//   +9: 2^(9/12)   = 1.6818
	17818,	//  +10: 2^(10/12)  = 1.7818
	18877,	//  +11: 2^(11/12)  = 1.8877
	20000,	//  +12: 2^(12/12)  = 2.0000
};

// inverse: 1/pitch_ratio * 10000 (tempo compensation)
//   index 0  = -12 semitones = 2.0000x tempo
//   index 12 =   0 semitones = 1.0000x tempo
//   index 24 = +12 semitones = 0.5000x tempo
static const uint32_t tempo_ratio_table[25] = {
	20000,	//  -12: 1/0.5000 = 2.0000
	18877,	//  -11: 1/0.5297 = 1.8877
	17818,	//  -10: 1/0.5612 = 1.7818
	16818,	//   -9: 1/0.5946 = 1.6818
	15874,	//   -8: 1/0.6300 = 1.5874
	14983,	//   -7: 1/0.6674 = 1.4983
	14142,	//   -6: 1/0.7071 = 1.4142
	13348,	//   -5: 1/0.7492 = 1.3348
	12599,	//   -4: 1/0.7937 = 1.2599
	11892,	//   -3: 1/0.8409 = 1.1892
	11225,	//   -2: 1/0.8909 = 1.1225
	10595,	//   -1: 1/0.9439 = 1.0595
	10000,	//    0: 1/1.0000 = 1.0000
	 9439,	//   +1: 1/1.0595 = 0.9439
	 8909,	//   +2: 1/1.1225 = 0.8909
	 8409,	//   +3: 1/1.1892 = 0.8409
	 7937,	//   +4: 1/1.2599 = 0.7937
	 7492,	//   +5: 1/1.3348 = 0.7492
	 7071,	//   +6: 1/1.4142 = 0.7071
	 6674,	//   +7: 1/1.4983 = 0.6674
	 6300,	//   +8: 1/1.5874 = 0.6300
	 5946,	//   +9: 1/1.6818 = 0.5946
	 5612,	//  +10: 1/1.7818 = 0.5612
	 5297,	//  +11: 1/1.8877 = 0.5297
	 5000,	//  +12: 1/2.0000 = 0.5000
};

// globals
static vod_hash_t key_change_filter_hash;

static uint32_t
key_change_filter_get_desc_size(media_clip_t* clip)
{
	return KEY_CHANGE_FILTER_MAX_DESC_SIZE;
}

static u_char*
key_change_filter_append_desc(u_char* p, media_clip_t* clip)
{
	media_clip_key_change_filter_t* filter = vod_container_of(clip, media_clip_key_change_filter_t, base);
	uint32_t pitch;
	uint32_t tempo;
	int idx;

	idx = filter->semitones - KEY_CHANGE_MIN_SEMITONES;
	pitch = pitch_ratio_table[idx];
	tempo = tempo_ratio_table[idx];

	// output: [src]asetrate=SR*P.PPPP,aresample=SR,atempo=T.TTTT[dst]
	// SR is hardcoded to 44100 — the standard sample rate for music.
	// asetrate changes the declared sample rate (shifting pitch),
	// aresample resamples back to 44100,
	// atempo compensates the speed change.
	return vod_sprintf(
		p,
		KEY_CHANGE_FILTER_DESC_PATTERN,
		clip->sources[0]->id,
		(uint32_t)44100,
		pitch / 10000,
		pitch % 10000,
		(uint32_t)44100,
		tempo / 10000,
		tempo % 10000,
		clip->id);
}

static audio_filter_t key_change_filter = {
	key_change_filter_get_desc_size,
	key_change_filter_append_desc,
};

vod_status_t
key_change_filter_parse(
	void* ctx,
	vod_json_object_t* element,
	void** result)
{
	media_filter_parse_context_t* context = ctx;
	media_clip_key_change_filter_t* filter;
	vod_json_value_t* params[KEY_CHANGE_FILTER_PARAM_COUNT];
	vod_json_value_t* source;
	vod_json_value_t* semitones;
	int32_t semitone_val;
	vod_status_t rc;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"key_change_filter_parse: started");

	vod_memzero(params, sizeof(params));

	vod_json_get_object_values(
		element,
		&key_change_filter_hash,
		params);

	semitones = params[KEY_CHANGE_FILTER_PARAM_SEMITONES];
	source = params[KEY_CHANGE_FILTER_PARAM_SOURCE];

	if (semitones == NULL || source == NULL)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"key_change_filter_parse: \"semitones\" and \"source\" are mandatory for keyChange filter");
		return VOD_BAD_MAPPING;
	}

	semitone_val = (int32_t)semitones->v.num.num;

	if (semitone_val < KEY_CHANGE_MIN_SEMITONES || semitone_val > KEY_CHANGE_MAX_SEMITONES)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"key_change_filter_parse: invalid semitones %i, must be between %i and %i",
			semitone_val, KEY_CHANGE_MIN_SEMITONES, KEY_CHANGE_MAX_SEMITONES);
		return VOD_BAD_MAPPING;
	}

	if (semitone_val == 0)
	{
		// no pitch change — just parse and return the source directly
		return media_set_parse_clip(
			context,
			&source->v.obj,
			NULL,
			result);
	}

	filter = vod_alloc(context->request_context->pool, sizeof(*filter) + sizeof(filter->base.sources[0]));
	if (filter == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"key_change_filter_parse: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	filter->base.sources = (void*)(filter + 1);
	filter->base.source_count = 1;

	filter->base.type = MEDIA_CLIP_KEY_CHANGE_FILTER;
	filter->base.audio_filter = &key_change_filter;
	filter->semitones = semitone_val;

	rc = media_set_parse_clip(
		context,
		&source->v.obj,
		&filter->base,
		&filter->base.sources[0]);
	if (rc != VOD_JSON_OK)
	{
		return rc;
	}

	*result = &filter->base;

	vod_log_debug1(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"key_change_filter_parse: done, semitones=%i", filter->semitones);

	return VOD_OK;
}

vod_status_t
key_change_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool)
{
	vod_status_t rc;

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"key_change_filter_hash",
		key_change_filter_params,
		sizeof(key_change_filter_params[0]),
		&key_change_filter_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}
