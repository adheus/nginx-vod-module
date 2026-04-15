#ifndef __KEY_CHANGE_FILTER_H__
#define __KEY_CHANGE_FILTER_H__

// includes
#include "../media_set.h"
#include "../json_parser.h"

// typedefs
typedef struct {
	media_clip_t base;
	int32_t semitones;
} media_clip_key_change_filter_t;

// functions
vod_status_t key_change_filter_parse(
	void* context,
	vod_json_object_t* element,
	void** result);

vod_status_t key_change_filter_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool);

#endif // __KEY_CHANGE_FILTER_H__
