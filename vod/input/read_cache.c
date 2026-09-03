#include "read_cache.h"
#include "../media_clip.h"

#define MIN_BUFFER_COUNT (2)

void 
read_cache_init(read_cache_state_t* state, request_context_t* request_context, size_t buffer_size)
{
	state->request_context = request_context;
	state->buffer_size = buffer_size;
	state->buffer_count = 0;
	state->reuse_buffers = TRUE;
	state->target_buffer = NULL;
	state->wait_buffer = NULL;
}

vod_status_t
read_cache_allocate_buffer_slots(read_cache_state_t* state, size_t buffer_count)
{
	size_t alloc_size;

	if (buffer_count < MIN_BUFFER_COUNT)
	{
		buffer_count = MIN_BUFFER_COUNT;
	}

	// headroom for read-ahead: the leaf-source count gives every source one slot,
	// which is exactly one outstanding read per source. extra slots let one source
	// hold several byte ranges at once (structs only - data buffers are lazy)
	buffer_count += READ_CACHE_READ_AHEAD_SLOTS;

	if (state->buffer_count >= buffer_count)
	{
		return VOD_OK;
	}

	alloc_size = sizeof(state->buffers[0]) * buffer_count;

	state->buffers = vod_alloc(state->request_context->pool, alloc_size);
	if (state->buffers == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"read_cache_allocate_buffer_slots: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	state->buffers_end = state->buffers + buffer_count;
	state->buffer_count = buffer_count;

	vod_memzero(state->buffers, alloc_size);

	return VOD_OK;
}

bool_t 
read_cache_get_from_cache(
	read_cache_state_t* state, 
	read_cache_request_t* request,
	u_char** buffer,
	uint32_t* size)
{
	media_clip_source_t* source = request->source;
	read_cache_hint_t* hint;
	cache_buffer_t* target_buffer;
	cache_buffer_t* cur_buffer;
	uint32_t read_size;
	uint64_t cur_end_offset;
	uint64_t aligned_last_offset;
	uint64_t offset = request->cur_offset;
	size_t alignment;
	int cache_slot_id;

	// record where the consumer of this source is - every slot of the source whose
	// data ends at or before this offset is behind the consumer and free for reuse
	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (cur_buffer->source == source)
		{
			cur_buffer->consumer_offset = offset;
		}
	}

	// check whether we already have the requested offset
	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (cur_buffer->in_flight)
		{
			// end_offset is stale until the read completes
			continue;
		}

		if (cur_buffer->source == source &&
			offset >= cur_buffer->start_offset && offset < cur_buffer->end_offset)
		{
			*buffer = cur_buffer->buffer_pos + (offset - cur_buffer->start_offset);
			*size = cur_buffer->end_offset - offset;
			return TRUE;
		}
	}

	// check whether the requested offset is already being read
	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (cur_buffer->in_flight &&
			cur_buffer->source == source &&
			offset >= cur_buffer->start_offset &&
			offset < cur_buffer->start_offset + cur_buffer->buffer_size)
		{
			// miss, but nothing to issue - the caller has to wait for the read to complete
			state->target_buffer = NULL;
			state->wait_buffer = cur_buffer;
			return FALSE;
		}
	}

	// don't have the offset in cache
	alignment = source->alignment - 1;
	cache_slot_id = request->cache_slot_id;

	// start reading from the min offset, if that would contain the whole frame
	// Note: this condition is intended to optimize the case in which the frame order 
	//		in the output segment is <video1><audio1> while on disk it's <audio1><video1>. 
	//		in this case it would be better to start reading from the beginning, even 
	//		though the first frame that is requested is the second one
	hint = &request->hint;
	if (hint->min_offset < offset && 
		hint->min_offset + state->buffer_size / 4 > offset &&
		request->end_offset < (hint->min_offset & ~alignment) + state->buffer_size)
	{
		offset = hint->min_offset;
		cache_slot_id = hint->min_offset_slot_id;
	}
	offset &= ~alignment;

	// calculate the read size
	read_size = state->buffer_size;
	target_buffer = &state->buffers[cache_slot_id % state->buffer_count];

	if (target_buffer->in_flight)
	{
		// the slot is busy with another read - the caller has to wait for it to complete
		state->target_buffer = NULL;
		state->wait_buffer = target_buffer;
		return FALSE;
	}

	// don't read anything that is already in the cache
	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (cur_buffer == target_buffer ||
			cur_buffer->source != source)
		{
			continue;
		}

		// for in-flight buffers end_offset is stale - use the intended read extent
		cur_end_offset = cur_buffer->in_flight ?
			cur_buffer->start_offset + cur_buffer->buffer_size :
			cur_buffer->end_offset;

		if (cur_buffer->start_offset > offset)
		{
			read_size = vod_min(read_size, cur_buffer->start_offset - offset);
		}
		else if (cur_end_offset > offset)
		{
			offset = cur_end_offset & ~alignment;
		}
	}

	// don't read past the max required offset
	if (offset + read_size > source->last_offset)
	{
		aligned_last_offset = (source->last_offset + alignment) & ~alignment;
		if (aligned_last_offset > offset)
		{
			read_size = aligned_last_offset - offset;
		}
	}

	target_buffer->source = source;
	target_buffer->start_offset = offset;
	target_buffer->buffer_size = read_size;
	target_buffer->consumer_offset = request->cur_offset;
	state->target_buffer = target_buffer;

	return FALSE;
}

// a slot is free for read-ahead when nothing is in flight on it and its data (if any)
// is behind its source's consumer. never an in-flight slot, never data still ahead
// of the consumer - read-ahead does not clobber
static bool_t
read_cache_is_slot_free(cache_buffer_t* cur_buffer)
{
	if (cur_buffer->in_flight)
	{
		return FALSE;
	}

	if (cur_buffer->source == NULL)
	{
		return TRUE;
	}

	return cur_buffer->end_offset <= cur_buffer->consumer_offset;
}

bool_t
read_cache_prepare_read_ahead(
	read_cache_state_t* state,
	media_clip_source_t* source,
	uint64_t from_offset)
{
	cache_buffer_t* target_buffer = NULL;
	cache_buffer_t* cur_buffer;
	uint64_t cur_end_offset;
	uint64_t aligned_last_offset;
	uint64_t next_offset;
	uint64_t offset;
	uint32_t read_size;
	size_t alignment;
	size_t pass;
	bool_t advanced;

	if (state->target_buffer != NULL || state->buffer_count == 0)
	{
		// a demand read is pending - it must be issued first
		return FALSE;
	}

	// pick a free slot, preferring one that already owns a data buffer so that
	// read-ahead does not grow the request's memory footprint when it can avoid it.
	// chosen first so that its (consumed) contents are ignored below - they are
	// about to be overwritten either way
	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (!read_cache_is_slot_free(cur_buffer))
		{
			continue;
		}

		if (target_buffer == NULL ||
			(target_buffer->buffer_start == NULL && cur_buffer->buffer_start != NULL))
		{
			target_buffer = cur_buffer;
		}
	}

	if (target_buffer == NULL)
	{
		// every slot is busy or holds data still ahead of its consumer - do nothing
		return FALSE;
	}

	alignment = source->alignment - 1;
	offset = from_offset & ~alignment;

	// skip past every range of this source that is cached or in flight, starting
	// at from_offset. each pass advances past at least one range; a pass that
	// fails to advance (unaligned tail rounding back into the same range) or
	// runs out of passes means there is nothing contiguous left to read ahead
	for (pass = 0; ; pass++)
	{
		if (pass > state->buffer_count)
		{
			return FALSE;
		}

		advanced = FALSE;
		for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
		{
			if (cur_buffer == target_buffer ||
				cur_buffer->source != source)
			{
				continue;
			}

			cur_end_offset = cur_buffer->in_flight ?
				cur_buffer->start_offset + cur_buffer->buffer_size :
				cur_buffer->end_offset;

			if (cur_buffer->start_offset <= offset && offset < cur_end_offset)
			{
				next_offset = cur_end_offset & ~alignment;
				if (next_offset <= offset)
				{
					return FALSE;
				}

				offset = next_offset;
				advanced = TRUE;
			}
		}

		if (!advanced)
		{
			break;
		}
	}

	// nothing beyond the last byte this request needs from the source
	if (offset >= source->last_offset)
	{
		return FALSE;
	}

	aligned_last_offset = (source->last_offset + alignment) & ~alignment;
	read_size = vod_min((uint64_t)state->buffer_size, aligned_last_offset - offset);

	// stop short of the next range of this source that is already cached / in flight
	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (cur_buffer != target_buffer &&
			cur_buffer->source == source &&
			cur_buffer->start_offset > offset)
		{
			read_size = vod_min(read_size, cur_buffer->start_offset - offset);
		}
	}

	if (read_size == 0)
	{
		return FALSE;
	}

	target_buffer->source = source;
	target_buffer->start_offset = offset;
	target_buffer->buffer_size = read_size;
	target_buffer->consumer_offset = from_offset;
	state->target_buffer = target_buffer;

	return TRUE;
}

void
read_cache_release_buffers(read_cache_state_t* state)
{
	cache_buffer_t* cur_buffer;

	for (cur_buffer = state->buffers; cur_buffer < state->buffers_end; cur_buffer++)
	{
		if (cur_buffer->in_flight)
		{
			// never expected here (callers drain first), but a slot with a read
			// landing on it must keep its identity so the completion is routed
			continue;
		}

		cur_buffer->source = NULL;
		cur_buffer->start_offset = 0;
		cur_buffer->end_offset = 0;
		cur_buffer->consumer_offset = 0;
		// buffer_start / buffer_pos are kept - the allocation is reused
	}
}

void
read_cache_disable_buffer_reuse(read_cache_state_t* state)
{
	state->reuse_buffers = FALSE;
}

void 
read_cache_get_read_buffer(
	read_cache_state_t* state, 
	read_cache_get_read_buffer_t* result)
{
	cache_buffer_t* target_buffer = state->target_buffer;

	// return the target buffer pointer and size
	result->source = target_buffer->source;
	result->offset = target_buffer->start_offset;
	result->buffer = state->reuse_buffers ? target_buffer->buffer_start : NULL;
	result->size = target_buffer->buffer_size;

	// the read is being issued - mark the slot busy and consume the demand
	target_buffer->in_flight = TRUE;
	state->target_buffer = NULL;
}

void
read_cache_read_completed_ex(read_cache_state_t* state, cache_buffer_t* target, vod_buf_t* buf)
{
	// update the buffer size
	target->buffer_start = buf->start;
	target->buffer_pos = buf->pos;
	target->buffer_size = buf->last - buf->pos;
	target->end_offset = target->start_offset + target->buffer_size;

	// the read for this slot has landed
	target->in_flight = FALSE;
}
