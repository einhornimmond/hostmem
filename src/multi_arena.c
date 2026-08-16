#include "hostmem/multi_arena.h"

#include "hostmem/bucket_vector.h"
#include "hostmem/memory.h"
#include "hostmem/result.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * A chain of bump arenas. Each arena is an ordinary hostmem in owned or external mode, so every
 * rule the single arena carries — alignment, sizes passed in, tail-only reclaim — holds inside
 * one stretch and is not reimplemented here. What this file adds is the order between the
 * stretches and the marker that keeps the search short.
 *
 * Two invariants:
 *
 *  - The chain is append only. An arena keeps its index for as long as it exists, so the
 *    addresses it handed out stay meaningful and `first_open` stays comparable across calls.
 *    Only the tail of the chain is ever removed, by _shrink, and only while it holds nothing.
 *  - `first_open` never points past an arena that could still serve a request. It walks forward
 *    over arenas that have fallen full, and walks back only when _free reopens one.
 *
 * The descriptors are stored by value in a bucket vector: buckets never move, so a `hostmem *`
 * taken from the vector survives the chain growing under it. A reallocated array would not.
 */

/** The descriptor vector, generated once here for the whole library. */
HOSTMEM_BVEC_DEFINE(hostmem_arena_vec, hostmem, HOSTMEM_MULTI_ARENA_BUCKET_LOG2, )

// the capacity a fresh regular arena is opened with; 0 in the field means "the default", so a
// zero-initialized descriptor allocates exactly like an initialized one
static uint32_t regular_capacity(const hostmem_multi_arena *m) {
  return m->arena_capacity ? m->arena_capacity : HOSTMEM_MULTI_ARENA_DEFAULT_CAPACITY;
}

// the remainder this chain writes an arena off at; 0 in the field means "the default", read the
// same way as arena_capacity so that a zero-initialized descriptor behaves like an initialized
// one. The caller sets it at init from the request sizes it expects.
static uint32_t full_threshold(const hostmem_multi_arena *m) {
  return m->full_remaining ? m->full_remaining : HOSTMEM_MULTI_ARENA_DEFAULT_FULL_REMAINING;
}

// bytes still to be had from this arena. last_index never passes capacity, so this cannot wrap
static uint32_t remaining(const hostmem *arena) {
  return arena->capacity - arena->last_index;
}

// An arena whose remainder has fallen this low is done: the front marker passes it and never
// looks back. A threshold rather than "nothing left", because the last few bytes of an arena
// would otherwise keep the scan walking over it for the rest of the chain's life. Where that
// line sits is the caller's call — see full_threshold above.
static bool has_run_full(const hostmem *arena, uint32_t threshold) {
  return remaining(arena) <= threshold;
}

// Append an arena that is already initialized. On failure the descriptor is released, so the
// caller's stack copy never outlives the buffer it points at.
static hostmem_result push_arena(hostmem_multi_arena *m, hostmem *arena) {
  hostmem_result result = hostmem_arena_vec_push_ptr(&m->arenas, arena);
  if (HOSTMEM_SUCCESS != result) {
    hostmem_release(arena);
    return result;
  }
  return HOSTMEM_SUCCESS;
}

// ********** manage the allocator itself *******************

hostmem_result hostmem_multi_arena_init(
    hostmem_multi_arena *m, uint32_t arena_capacity, uint32_t full_remaining, hostmem *bookkeeping
) {
  if (!m) { return HOSTMEM_ERROR_NULL_POINTER; }
  uint32_t aligned_capacity;
  if (!hostmem_align8_u32(arena_capacity, &aligned_capacity)) {
    return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW;
  }

  // A threshold that reaches the capacity would call every arena full the moment it opens: the
  // marker would walk past fresh ground and every allocation would get an arena of its own. The
  // effective values are compared, so a 0 on either side means what it means everywhere else.
  const uint32_t capacity =
      aligned_capacity ? aligned_capacity : HOSTMEM_MULTI_ARENA_DEFAULT_CAPACITY;
  const uint32_t threshold =
      full_remaining ? full_remaining : HOSTMEM_MULTI_ARENA_DEFAULT_FULL_REMAINING;
  if (threshold >= capacity) { return HOSTMEM_ERROR_INVALID_PARAM; }

  // every field is written, none is read: uninitialized storage is a valid input
  m->arena_capacity = aligned_capacity;
  m->full_remaining = full_remaining;
  m->first_open = 0;
  return hostmem_arena_vec_init(&m->arenas, bookkeeping);
}

hostmem_multi_arena *hostmem_multi_arena_create(
    uint32_t arena_capacity, uint32_t full_remaining, hostmem *bookkeeping
) {
  hostmem_multi_arena *m = NULL;
  if (HOSTMEM_SUCCESS != hostmem_alloc((uint8_t **)&m, sizeof(hostmem_multi_arena), NULL)) {
    return NULL;
  }
  if (HOSTMEM_SUCCESS != hostmem_multi_arena_init(m, arena_capacity, full_remaining, bookkeeping)) {
    hostmem_free((uint8_t *)m, sizeof(hostmem_multi_arena), NULL);
    return NULL;
  }
  return m;
}

hostmem_result hostmem_multi_arena_reserve(hostmem_multi_arena *m, uint32_t arena_count) {
  if (!m) { return HOSTMEM_ERROR_NULL_POINTER; }
  return hostmem_arena_vec_reserve(&m->arenas, arena_count);
}

hostmem_result hostmem_multi_arena_borrow(
    hostmem_multi_arena *m, uint8_t *data, uint32_t capacity
) {
  if (!m || !data) { return HOSTMEM_ERROR_NULL_POINTER; }

  // the borrowed block is checked by the arena itself; nothing is appended if it is unfit
  hostmem arena;
  hostmem_result result = hostmem_init_arena_borrow(&arena, data, capacity);
  if (HOSTMEM_SUCCESS != result) { return result; }

  return push_arena(m, &arena);
}

void hostmem_multi_arena_reset(hostmem_multi_arena *m) {
  if (!m) return;
  hostmem *arena;
  HOSTMEM_BVEC_FOREACH(hostmem_arena_vec, &m->arenas, arena, i) {
    hostmem_reset(arena);
  }
  m->first_open = 0;
}

hostmem_result hostmem_multi_arena_shrink(hostmem_multi_arena *m) {
  if (!m) { return HOSTMEM_ERROR_NULL_POINTER; }

  // youngest first, and stop at the first arena that is still holding something or that we do
  // not own. Removing from the middle would renumber the chain for nothing gained.
  hostmem *tail;
  while ((tail = hostmem_arena_vec_back(&m->arenas)) != NULL) {
    if (tail->last_index || HOSTMEM_ALLOC_TYPE_ARENA_OWNED != tail->allocation_type) { break; }
    hostmem_release(tail);
    (void)hostmem_arena_vec_pop(&m->arenas);
  }

  uint32_t count = hostmem_arena_vec_size(&m->arenas);
  if (m->first_open > count) { m->first_open = count; }
  return hostmem_arena_vec_shrink(&m->arenas);
}

void hostmem_multi_arena_release(hostmem_multi_arena *m) {
  if (!m) return;
  // owned arenas give their buffer back, borrowed ones are simply let go
  hostmem *arena;
  HOSTMEM_BVEC_FOREACH(hostmem_arena_vec, &m->arenas, arena, i) {
    hostmem_release(arena);
  }
  // leaves the vector in its empty state with the bookkeeping allocator still attached
  hostmem_arena_vec_free(&m->arenas);
  m->first_open = 0;
}

void hostmem_multi_arena_destroy(hostmem_multi_arena *m) {
  if (!m) return;
  hostmem_multi_arena_release(m);
  hostmem_free((uint8_t *)m, sizeof(hostmem_multi_arena), NULL);
}

hostmem_result hostmem_multi_arena_measure(
    const hostmem_multi_arena *m, hostmem_multi_arena_stats *out
) {
  if (!m || !out) { return HOSTMEM_ERROR_NULL_POINTER; }

  // uint64 for the sums: a single arena is measured in uint32_t, a chain of them is not
  hostmem_multi_arena_stats stats = {0, 0, 0, 0};
  const uint32_t threshold = full_threshold(m);
  hostmem *arena;
  HOSTMEM_BVEC_FOREACH(hostmem_arena_vec, &m->arenas, arena, i) {
    stats.reserved += arena->capacity;
    stats.used += arena->last_index;
    if (!has_run_full(arena, threshold)) { stats.open_count++; }
  }
  stats.arena_count = hostmem_arena_vec_size(&m->arenas);

  *out = stats;
  return HOSTMEM_SUCCESS;
}

// ********** allocations, with data ptr and size explicit *******************

hostmem_result hostmem_multi_arena_alloc(uint8_t **buffer, uint32_t size, hostmem_multi_arena *m) {
  if (!buffer || !m) { return HOSTMEM_ERROR_NULL_POINTER; }
  if (!size) { return HOSTMEM_ERROR_INVALID_PARAM; }
  uint32_t needed;
  if (!hostmem_align8_u32(size, &needed)) { return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW; }

  const uint32_t count = hostmem_arena_vec_size(&m->arenas);
  const uint32_t threshold = full_threshold(m);

  // the front marker settles past everything that has run full, once, instead of every scan
  // walking the same exhausted arenas again. How much room an arena may still hold and be passed
  // over anyway is the chain's own threshold — the caller sized it for these requests.
  while (m->first_open < count &&
         has_run_full(hostmem_arena_vec_get(&m->arenas, m->first_open), threshold)) {
    m->first_open++;
  }

  // first fit. An arena that cannot hold this request may still hold the next, smaller one, so
  // it is skipped, not closed.
  for (uint32_t i = m->first_open; i < count; ++i) {
    hostmem *arena = hostmem_arena_vec_get(&m->arenas, i);
    if (remaining(arena) < needed) { continue; }
    return hostmem_alloc(buffer, size, arena);
  }

  // nothing had room: open fresh ground. A request larger than a regular arena gets one sized
  // exactly for it — full on arrival, and out of the way of every later request.
  uint32_t capacity = regular_capacity(m);
  if (capacity < needed) { capacity = needed; }

  hostmem arena;
  hostmem_result result = hostmem_init_arena(&arena, capacity);
  if (HOSTMEM_SUCCESS != result) { return result; }
  result = push_arena(m, &arena);
  if (HOSTMEM_SUCCESS != result) { return result; }

  // the push copied the descriptor; the copy in the vector is the owner from here on
  return hostmem_alloc(buffer, size, hostmem_arena_vec_back(&m->arenas));
}

hostmem_result hostmem_multi_arena_clone(
    uint8_t **dst_buffer, const uint8_t *src, uint32_t size, hostmem_multi_arena *m
) {
  if (!dst_buffer || !src || !m) { return HOSTMEM_ERROR_NULL_POINTER; }
  if (!size) { return HOSTMEM_ERROR_INVALID_PARAM; }

  hostmem_result result = hostmem_multi_arena_alloc(dst_buffer, size, m);
  if (HOSTMEM_SUCCESS != result) { return result; }

  // copy the requested size, not what the arena reserved for it
  memcpy(*dst_buffer, src, size);
  return HOSTMEM_SUCCESS;
}

hostmem_result hostmem_multi_arena_free(uint8_t *buffer, uint32_t size, hostmem_multi_arena *m) {
  if (!m) { return HOSTMEM_ERROR_NULL_POINTER; }
  // NULL is never a tail, and the single arena answers the same way
  if (!buffer) { return HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED; }

  const uint32_t count = hostmem_arena_vec_size(&m->arenas);
  for (uint32_t i = 0; i < count; ++i) {
    hostmem *arena = hostmem_arena_vec_get(&m->arenas, i);
    if (buffer < arena->data || buffer >= arena->data + arena->capacity) { continue; }

    hostmem_result result = hostmem_free(buffer, size, arena);
    // an arena that took bytes back may have room again; the marker follows it back
    if (HOSTMEM_SUCCESS == result && i < m->first_open) { m->first_open = i; }
    return result;
  }

  // no arena covers this address: it was not handed out by this chain
  return HOSTMEM_ERROR_INVALID_PARAM;
}
