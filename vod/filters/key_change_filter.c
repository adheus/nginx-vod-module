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

// Cents bound. +-1200 cents == +-12 semitones == pitch ratio 0.5 .. 2.0, which
// is exactly the span a single atempo stage can undo (atempo accepts 0.5-2.0).
// Going wider would need chained atempo stages, so callers must fold musical
// key shift and tempo compensation together and keep the total inside this.
#define KEY_CHANGE_MIN_CENTS (-1200)
#define KEY_CHANGE_MAX_CENTS (1200)

// enums
enum {
	KEY_CHANGE_FILTER_PARAM_SEMITONES,
	KEY_CHANGE_FILTER_PARAM_CENTS,
	KEY_CHANGE_FILTER_PARAM_SOURCE,

	KEY_CHANGE_FILTER_PARAM_COUNT
};

// constants
static json_object_key_def_t key_change_filter_params[] = {
	{ vod_string("semitones"),	VOD_JSON_INT,		KEY_CHANGE_FILTER_PARAM_SEMITONES },
	{ vod_string("cents"),		VOD_JSON_INT,		KEY_CHANGE_FILTER_PARAM_CENTS },
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

// 2^(r/1200) * 1e6 for the leftover cents r in [0,100). Combined with
// pitch_ratio_table above this gives 2^(cents/1200) without any floating
// point: 2^(cents/1200) = 2^(s/12) * 2^(r/1200) where cents = s*100 + r.
// The module links no libm and is integer-only by design, hence tables.
static const uint32_t cent_ratio_table[100] = {
	1000000,	// + 0c = 1.000000
	1000578,	// + 1c
	1001156,	// + 2c
	1001734,	// + 3c
	1002313,	// + 4c
	1002892,	// + 5c
	1003472,	// + 6c
	1004052,	// + 7c
	1004632,	// + 8c
	1005212,	// + 9c
	1005793,	// +10c = 1.005793
	1006374,	// +11c
	1006956,	// +12c
	1007537,	// +13c
	1008120,	// +14c
	1008702,	// +15c
	1009285,	// +16c
	1009868,	// +17c
	1010451,	// +18c
	1011035,	// +19c
	1011619,	// +20c = 1.011619
	1012204,	// +21c
	1012789,	// +22c
	1013374,	// +23c
	1013959,	// +24c
	1014545,	// +25c
	1015132,	// +26c
	1015718,	// +27c
	1016305,	// +28c
	1016892,	// +29c
	1017480,	// +30c = 1.017480
	1018068,	// +31c
	1018656,	// +32c
	1019244,	// +33c
	1019833,	// +34c
	1020423,	// +35c
	1021012,	// +36c
	1021602,	// +37c
	1022192,	// +38c
	1022783,	// +39c
	1023374,	// +40c = 1.023374
	1023965,	// +41c
	1024557,	// +42c
	1025149,	// +43c
	1025741,	// +44c
	1026334,	// +45c
	1026927,	// +46c
	1027520,	// +47c
	1028114,	// +48c
	1028708,	// +49c
	1029302,	// +50c = 1.029302
	1029897,	// +51c
	1030492,	// +52c
	1031087,	// +53c
	1031683,	// +54c
	1032279,	// +55c
	1032876,	// +56c
	1033472,	// +57c
	1034070,	// +58c
	1034667,	// +59c
	1035265,	// +60c = 1.035265
	1035863,	// +61c
	1036462,	// +62c
	1037060,	// +63c
	1037660,	// +64c
	1038259,	// +65c
	1038859,	// +66c
	1039459,	// +67c
	1040060,	// +68c
	1040661,	// +69c
	1041262,	// +70c = 1.041262
	1041864,	// +71c
	1042466,	// +72c
	1043068,	// +73c
	1043671,	// +74c
	1044274,	// +75c
	1044877,	// +76c
	1045481,	// +77c
	1046085,	// +78c
	1046689,	// +79c
	1047294,	// +80c = 1.047294
	1047899,	// +81c
	1048505,	// +82c
	1049111,	// +83c
	1049717,	// +84c
	1050323,	// +85c
	1050930,	// +86c
	1051537,	// +87c
	1052145,	// +88c
	1052753,	// +89c
	1053361,	// +90c = 1.053361
	1053970,	// +91c
	1054579,	// +92c
	1055188,	// +93c
	1055798,	// +94c
	1056408,	// +95c
	1057018,	// +96c
	1057629,	// +97c
	1058240,	// +98c
	1058851,	// +99c
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
	uint32_t cent_6dp;
	int32_t semis;
	int32_t rem;
	int idx;

	// Split the shift into whole semitones plus leftover cents, so the
	// ratio comes from two integer tables instead of libm. Floor the
	// division: C truncates toward zero, which would put r negative.
	semis = filter->cents / 100;
	rem = filter->cents % 100;
	if (rem < 0)
	{
		rem += 100;
		semis -= 1;
	}

	idx = semis - KEY_CHANGE_MIN_SEMITONES;
	pitch_4dp = pitch_ratio_table[idx];
	cent_6dp = cent_ratio_table[rem];

	// Compute the exact integer asetrate ffmpeg will apply. Going
	// through the int math here means the value we ship in the filter
	// description matches the value ffmpeg uses internally — no
	// surprises from float-to-int truncation inside the filter
	// expression evaluator.
	//   44100 * (pitch_4dp/1e4) * (cent_6dp/1e6)
	// Worst case 44100 * 20000 * 1057018 ~ 9.3e14, well inside uint64.
	asetrate_int = (uint32_t)((44100ULL * pitch_4dp * cent_6dp) / (10000ULL * 1000000ULL));

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
	vod_json_value_t* cents;
	int32_t semitone_val = 0;
	int32_t cent_val;
	vod_status_t rc;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"key_change_filter_parse: started");

	vod_memzero(params, sizeof(params));

	vod_json_get_object_values(
		element,
		&key_change_filter_hash,
		params);

	semitones = params[KEY_CHANGE_FILTER_PARAM_SEMITONES];
	cents = params[KEY_CHANGE_FILTER_PARAM_CENTS];
	source = params[KEY_CHANGE_FILTER_PARAM_SOURCE];

	if (source == NULL || (semitones == NULL && cents == NULL))
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"key_change_filter_parse: \"source\" and one of \"cents\"/\"semitones\" "
			"are mandatory for keyChange filter");
		return VOD_BAD_MAPPING;
	}

	// "cents" wins when both are present. Musical transposition is naturally
	// expressed in semitones; tempo compensation (which is what BPM targeting
	// needs) never lands on a whole one, so it uses cents.
	if (cents != NULL)
	{
		cent_val = (int32_t)cents->v.num.num;
	}
	else
	{
		semitone_val = (int32_t)semitones->v.num.num;
		if (semitone_val < KEY_CHANGE_MIN_SEMITONES || semitone_val > KEY_CHANGE_MAX_SEMITONES)
		{
			vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
				"key_change_filter_parse: invalid semitones %i, must be between %i and %i",
				semitone_val, KEY_CHANGE_MIN_SEMITONES, KEY_CHANGE_MAX_SEMITONES);
			return VOD_BAD_MAPPING;
		}
		cent_val = semitone_val * 100;
	}

	if (cent_val < KEY_CHANGE_MIN_CENTS || cent_val > KEY_CHANGE_MAX_CENTS)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"key_change_filter_parse: invalid cents %i, must be between %i and %i "
			"(a single atempo stage only spans pitch 0.5-2.0)",
			cent_val, KEY_CHANGE_MIN_CENTS, KEY_CHANGE_MAX_CENTS);
		return VOD_BAD_MAPPING;
	}

	if (cent_val == 0)
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
	filter->cents = cent_val;

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
		"key_change_filter_parse: done, cents=%i", filter->cents);

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
