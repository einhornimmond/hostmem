#include "hostmem/fixed_arena_pool.h"

#include "hostmem/memory.h"
#include "hostmem/result.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * One allocation holds the whole pool:
 *
 *   [ hostmem descriptor 0 .. N-1 ][ pad to 8 ][ buffer 0 ][ buffer 1 ] ... [ buffer N-1 ]
 *
 * The descriptors come first so that the block's address and the descriptor array's address are
 * the same pointer -- `arenas` is both, which is what lets _release() hand everything back
 * without storing a second pointer. Each buffer is `arena_capacity` bytes, that figure already
 * rounded up to 8, so every buffer starts 8 byte aligned and hostmem_init_arena_borrow() accepts
 * it as it is.
 *
 * The free list threads through the buffers rather than through the descriptors: a free arena's
 * bytes are dead until it is lent out, and its first sizeof(hostmem *) of them carry the address
 * of the next free arena. Written and read with memcpy, because a uint8_t buffer is not a
 * hostmem * and pretending otherwise is the kind of aliasing a sanitizer is right to complain
 * about. The pointer fits: capacities are rounded up to 8, and the assert below settles that 8
 * is enough on this target.
 */

static_assert(
    sizeof(hostmem *) <= 8, "the free list link has to fit in the smallest arena, which is 8 bytes"
);
static_assert(
  sizeof(hostmem) <= UINT16_MAX-1, "hostmem has an unreasonable size"
);
static_assert(
  HOSTMEM_ALIGN8(sizeof(hostmem)) == sizeof(hostmem), "hostmem struct must be 8-Byte aligned"
);

/** Where the buffers begin, measured from the front of the block. */
static uint64_t buffers_offset(uint16_t arena_count) {
  return (uint64_t)arena_count * sizeof(hostmem);
}

/** Bytes the single block occupies. Recomputed rather than stored; the inputs never change. */
static uint64_t block_bytes(uint16_t arena_count, uint32_t arena_capacity) {
  return buffers_offset(arena_count) + (uint64_t)arena_count * arena_capacity;
}

/** Read the link a free arena carries in the first bytes of its buffer. */
static hostmem *next_free(const hostmem *arena) {
  hostmem *next = NULL;
  memcpy(&next, arena->data, sizeof(next));
  return next;
}

/** Write the link into a free arena's buffer. Only ever called on an arena the pool holds. */
static void set_next_free(hostmem *arena, hostmem *next) {
  memcpy(arena->data, &next, sizeof(next));
}

/** True when @p arena is one of @p pool's, addressed exactly at a slot and not between two. */
static bool belongs_to(const hostmem_fixed_arena_pool *pool, const hostmem *arena) {
  if (arena < pool->arenas || arena >= pool->arenas + pool->arena_count) { return false; }
  const size_t offset = (size_t)((const uint8_t *)arena - (const uint8_t *)pool->arenas);
  return offset % sizeof(hostmem) == 0;
}

/** The state a pool is in before init and after release: owning nothing, promising nothing. */
static void forget_everything(hostmem_fixed_arena_pool *pool) {
  pool->arenas = NULL;
  pool->free_head = NULL;
  pool->arena_capacity = 0;
  pool->arena_count = 0;
  pool->acquired_count = 0;
}

// ********** manage the pool itself *******************

hostmem_result hostmem_fixed_arena_pool_init(
    hostmem_fixed_arena_pool *pool, uint32_t arena_capacity, uint16_t arena_count, hostmem *source
) {
  if (!pool) { return HOSTMEM_ERROR_NULL_POINTER; }
  if (!arena_capacity || !arena_count) { return HOSTMEM_ERROR_INVALID_PARAM; }

  uint32_t capacity;
  if (!hostmem_align8_u32(arena_capacity, &capacity)) { return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW; }

  uint32_t descriptors_size = (uint32_t)sizeof(hostmem) * (uint32_t)arena_count;
  if (descriptors_size >= HOSTMEM_MAX_ALLOC_SIZE || HOSTMEM_MAX_ALLOC_SIZE / descriptors_size >= capacity) {
    return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW;
  }

  uint32_t total = (uint32_t)sizeof(hostmem) * (uint32_t)arena_count * capacity;
  // one block for everything, so one call gets it and one call gives it back
  uint8_t *block = NULL;
  hostmem_result result = hostmem_alloc(&block, total, source);
  if (HOSTMEM_SUCCESS != result) { return result; }

  hostmem *descriptors = (hostmem *)block;
  uint8_t *buffers = block + buffers_offset(arena_count);

  // strung back to front, so the head is arena 0 and a run of allocations walks the block in
  // order -- the same work either way, and far easier to follow in a debugger

  hostmem *head = NULL;
  for (uint32_t i = arena_count; i > 0; --i) {
    hostmem *arena = &descriptors[i - 1];
    uint8_t *buffer = buffers + (uint64_t)(i - 1) * capacity;

    result = hostmem_init_arena_borrow(arena, buffer, capacity);
    if (HOSTMEM_SUCCESS != result) {
      // unreachable with a layout this file laid out itself; handled rather than assumed, and
      // the block goes straight back so a failure leaves nothing behind
      hostmem_free(block, (uint32_t)total, source);
      return result;
    }
    set_next_free(arena, head);
    head = arena;
  }

  // nothing above can fail from here on, so the descriptor is written only now: a refused init
  // leaves whatever the caller had
  pool->arenas = descriptors;
  pool->free_head = head;
  pool->arena_capacity = capacity;
  pool->arena_count = arena_count;
  pool->acquired_count = 0;
  return HOSTMEM_SUCCESS;
}

hostmem_fixed_arena_pool *hostmem_fixed_arena_pool_create(
    uint32_t arena_capacity, uint16_t arena_count, hostmem *source, hostmem *allocator
) {
  hostmem_fixed_arena_pool *pool = NULL;
  // `allocator` carries this descriptor, `source` the arenas -- two questions, two answers
  if (HOSTMEM_SUCCESS !=
      hostmem_alloc((uint8_t **)&pool, sizeof(hostmem_fixed_arena_pool), allocator)) {
    return NULL;
  }
  if (HOSTMEM_SUCCESS != hostmem_fixed_arena_pool_init(pool, arena_capacity, arena_count, source)) {
    // straight back to where it came from; it is still the tail there, so an arena takes it
    hostmem_free((uint8_t *)pool, sizeof(hostmem_fixed_arena_pool), allocator);
    return NULL;
  }
  return pool;
}

hostmem_result hostmem_fixed_arena_pool_release(hostmem_fixed_arena_pool *pool, hostmem *source) {
  if (!pool) { return HOSTMEM_ERROR_NULL_POINTER; }
  // the one refusal that matters: an arena still out is memory someone is writing to
  if (pool->acquired_count) { return HOSTMEM_ERROR_RESOURCE_IN_USE; }
  if (!pool->arenas) {
    forget_everything(pool);
    return HOSTMEM_SUCCESS;
  }

  // the size is recomputed from what the pool holds, the allocator comes from the caller -- the
  // same split every free in this library uses, and the same duty it puts on the caller
  const uint32_t total = (uint32_t)block_bytes(pool->arena_count, pool->arena_capacity);
  uint8_t *block = (uint8_t *)pool->arenas;

  // emptied first: whatever the source answers, the pool has let go of the block and must not
  // be left pointing at it
  forget_everything(pool);
  return hostmem_free(block, total, source);
}

hostmem_result hostmem_fixed_arena_pool_destroy(
    hostmem_fixed_arena_pool *pool, hostmem *source, hostmem *allocator
) {
  // nothing to give back is not a failure
  if (!pool) { return HOSTMEM_SUCCESS; }

  const hostmem_result released = hostmem_fixed_arena_pool_release(pool, source);
  // the descriptor outlives a refusal on purpose: the caller still has arenas to return and
  // needs the pool to return them to
  if (HOSTMEM_ERROR_RESOURCE_IN_USE == released) { return released; }

  const hostmem_result freed =
      hostmem_free((uint8_t *)pool, sizeof(hostmem_fixed_arena_pool), allocator);
  // a warning from either step is worth more to the caller than the success of the other
  return (HOSTMEM_SUCCESS != released) ? released : freed;
}

uint32_t hostmem_fixed_arena_pool_reserved(const hostmem_fixed_arena_pool *pool) {
  if (!pool || !pool->arenas) { return 0; }
  return (uint32_t)block_bytes(pool->arena_count, pool->arena_capacity);
}

// ********** lend and take back *******************

hostmem_result hostmem_fixed_arena_pool_alloc(hostmem_fixed_arena_pool *pool, hostmem **out) {
  if (!pool || !out) { return HOSTMEM_ERROR_NULL_POINTER; }
  if (!pool->arenas) { return HOSTMEM_ERROR_NOT_INITIALIZED; }
  // the pool is the size it is; this says so rather than pretending the request was wrong
  if (!pool->free_head) { return HOSTMEM_ERROR_RESOURCE_EXHAUSTED; }

  hostmem *arena = pool->free_head;
  pool->free_head = next_free(arena);
  pool->acquired_count++;

  // the arena is already empty: _init left it so and _free resets on the way in. The link that
  // sat in its first bytes is simply overwritten by whatever the caller allocates.
  *out = arena;
  return HOSTMEM_SUCCESS;
}

hostmem_result hostmem_fixed_arena_pool_free(hostmem_fixed_arena_pool *pool, hostmem *arena) {
  if (!pool || !arena) { return HOSTMEM_ERROR_NULL_POINTER; }
  if (!pool->arenas) { return HOSTMEM_ERROR_NOT_INITIALIZED; }
  if (!belongs_to(pool, arena)) { return HOSTMEM_ERROR_INVALID_PARAM; }
  // nothing is out, so nothing can be coming back. The cheapest double return to catch, and the
  // only one a pool without a busy list can see at all.
  if (!pool->acquired_count) { return HOSTMEM_ERROR_INVALID_STATE; }

  // emptied before it rejoins the list, so the next caller gets what the first one got
  hostmem_reset(arena);
  set_next_free(arena, pool->free_head);
  pool->free_head = arena;
  pool->acquired_count--;
  return HOSTMEM_SUCCESS;
}
