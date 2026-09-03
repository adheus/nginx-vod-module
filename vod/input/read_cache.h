#ifndef __READ_CACHE_H__
#define __READ_CACHE_H__

// includes
#include "../common.h"

// typedefs
struct media_clip_source_s;

typedef struct {
	u_char* buffer_start;
	u_char* buffer_pos;
	uint32_t buffer_size;		// size of data read (holds the intended read size while in_flight)
	void* source;				// opaque context that indicates from where the buffer should be read
	uint64_t start_offset;
	uint64_t end_offset;
	bool_t in_flight;			// a read targeting this slot was issued and has not completed yet
								// (end_offset is stale until read_cache_read_completed_ex clears this)
	uint64_t consumer_offset;	// offset of the latest read_cache_get_from_cache request on this
								// slot's source. once end_offset <= consumer_offset the consumer has
								// moved past this slot's data and read-ahead may reuse the slot - the
								// same condition under which the demand path already overwrites it
} cache_buffer_t;

// slots added on top of the leaf-source count so that a single source can hold
// several byte ranges at once (demand read + read-ahead). only the slot structs
// are allocated up front - a slot's data buffer (cache_buffer_size) is allocated
// on its first use, and read-ahead prefers slots that already own a buffer
#define READ_CACHE_READ_AHEAD_SLOTS (3)

typedef struct {
	request_context_t* request_context;
	cache_buffer_t* buffers;
	cache_buffer_t* buffers_end;
	cache_buffer_t* target_buffer;
	cache_buffer_t* wait_buffer;	// when read_cache_get_from_cache misses without generating a read
									// demand, the in-flight slot the caller has to wait for
	size_t buffer_count;
	size_t buffer_size;
	bool_t reuse_buffers;
} read_cache_state_t;

typedef struct {
	uint64_t min_offset;
	int min_offset_slot_id;
} read_cache_hint_t;

typedef struct {
	int cache_slot_id;
	struct media_clip_source_s* source;
	uint64_t cur_offset;
	uint64_t end_offset;
	read_cache_hint_t hint;
} read_cache_request_t;

typedef struct {
	struct media_clip_source_s* source;
	uint64_t offset;
	u_char* buffer;
	uint32_t size;
} read_cache_get_read_buffer_t;

// functions
void read_cache_init(
	read_cache_state_t* state, 
	request_context_t* request_context, 
	size_t buffer_size);
	
vod_status_t read_cache_allocate_buffer_slots(
	read_cache_state_t* state,
	size_t buffer_count);

bool_t read_cache_get_from_cache(
	read_cache_state_t* state, 
	read_cache_request_t* request,
	u_char** buffer,
	uint32_t* size);

void read_cache_disable_buffer_reuse(
	read_cache_state_t* state);

// returns TRUE when read_cache_get_from_cache generated a read demand that was
// not consumed yet by read_cache_get_read_buffer
#define read_cache_has_pending_read(state) ((state)->target_buffer != NULL)

// maps a buffer slot pointer to its index within the cache
#define read_cache_buffer_index(state, buffer) ((int)((buffer) - (state)->buffers))

// Note: consumes state->target_buffer (sets it to NULL) and marks the slot in_flight;
//		capture state->target_buffer before calling in order to route the completion
void read_cache_get_read_buffer(
	read_cache_state_t* state,
	read_cache_get_read_buffer_t* result);

void read_cache_read_completed_ex(
	read_cache_state_t* state,
	cache_buffer_t* target,
	vod_buf_t* buf);

// prepares a read-ahead demand for source: the next byte range at/after from_offset
// that is neither cached nor in flight, bounded by source->last_offset, into a slot
// that is free (not in flight, and either unused or already consumed - see
// consumer_offset). on success returns TRUE with state->target_buffer set - the
// caller must issue it through read_cache_get_read_buffer exactly like a demand
// read. returns FALSE and touches nothing when the source needs nothing more, no
// slot is free, or a demand is already pending - read-ahead never clobbers
bool_t read_cache_prepare_read_ahead(
	read_cache_state_t* state,
	struct media_clip_source_s* source,
	uint64_t from_offset);

// marks every slot that is not in flight as empty, keeping its data buffer for
// reuse. called between the audio filter phase and the muxer phase - the filtered
// audio lives in memory by then, so the stems' slots become the read-ahead pool
// for the video source
void read_cache_release_buffers(
	read_cache_state_t* state);

#endif // __READ_CACHE_H__
