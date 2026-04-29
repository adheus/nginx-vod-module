#include "key_change_filter.h"
#include "audio_filter.h"
#include "../media_set_parser.h"

// macros
//   asetrate shifts pitch by changing the declared sample rate, aresample
//   restores the rate, atempo compensates the resulting speed change.
//
//   For the chain to preserve segment duration exactly (no cumulative
//   drift across HLS segments) we need:
//     pitch_actual × atempo_actual == 1 EXACTLY
//
//   ffmpeg's asetrate.sample_rate is AV_OPT_TYPE_INT, so the declared
//   rate is whatever integer we pass. We therefore (a) format asetrate
//   as a single integer (round(44100 * pitch_ratio)) so the value
//   ffmpeg actually applies is the value we computed, and (b) derive
//   atempo as the EXACT reciprocal (44100 / asetrate_int) at 6-decimal
//   precision so the residual error is <1 ppm per segment instead of
//   the ~25 ppm we got from the previous independently-rounded
//   pitch/tempo tables.
#define KEY_CHANGE_FILTER_DESC_PATTERN \
	"[%uD]asetrate=%uD,aresample=%uD,atempo=%uD.%06uD[%uD]"

// the filter desc carries 6 ints (id, asetrate, aresample, atempo
// whole, atempo frac, dst id) so allocate enough space for them all.
#define KEY_CHANGE_FILTER_MAX_DESC_SIZE \
	(sizeof(KEY_CHANGE_FILTER_DESC_PATTERN) + VOD_INT32_LEN * 6)

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

// (tempo_ratio_table removed — atempo is now derived exactly from the
//  integer asetrate value in key_change_filter_append_desc, so the
//  product pitch × tempo is 1 to within ~1 ppm instead of the ~25 ppm
//  we got from independently-rounded 4-decimal lookup tables.)

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
	uint32_t pitch_4dp;
	uint32_t asetrate_int;
	uint64_t atempo_6dp;
	uint32_t atempo_whole;
	uint32_t atempo_frac;
	int idx;

	idx = filter->semitones - KEY_CHANGE_MIN_SEMITONES;
	pitch_4dp = pitch_ratio_table[idx];

	// Compute the exact integer asetrate ffmpeg will apply. Going
	// through the int math here means the value we ship in the filter
	// description matches the value ffmpeg uses internally — no
	// surprises from float-to-int truncation inside the filter
	// expression evaluator.
	asetrate_int = (uint32_t)((44100ULL * pitch_4dp) / 10000ULL);

	// Derive atempo as the EXACT reciprocal of the pitch shift at
	// 6-decimal precision: atempo = 44100 / asetrate_int. This makes
	// pitch_actual × atempo_actual = 1 to within ~1 ppm, replacing
	// the previous ~25 ppm error from independently-rounded tables.
	// Without this, every segment was ~5 samples too long, which
	// compounded into ~10 ms drift per minute of playback (and
	// reset by seek — exactly the user-visible drift pattern).
	atempo_6dp = (44100ULL * 1000000ULL) / (uint64_t)asetrate_int;
	atempo_whole = (uint32_t)(atempo_6dp / 1000000ULL);
	atempo_frac  = (uint32_t)(atempo_6dp % 1000000ULL);

	// output: [src]asetrate=N,aresample=44100,atempo=T.TTTTTT[dst]
	return vod_sprintf(
		p,
		KEY_CHANGE_FILTER_DESC_PATTERN,
		clip->sources[0]->id,
		asetrate_int,
		(uint32_t)44100,
		atempo_whole,
		atempo_frac,
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
			(media_clip_t**)result);
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
