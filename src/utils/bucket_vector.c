#include "hostmem/utils/bucket_vector.h"

#include "hostmem/memory.h"
#include "hostmem/result.h"

/*
 * The generated containers keep only their type-dependent logic inline; the primitives below
 * carry the type-independent weight of allocation and exist exactly once, however many
 * payload types a program instantiates.
 *
 * Everything here counts in the allocator's uint32_t. Sizes that still had to be narrowed at
 * runtime are gone: a bucket's byte size is settled at compile time by the static assert in
 * HOSTMEM_BVEC_DECLARE, and the index array is the only place where a slot count still turns
 * into bytes — which happens below, once.
 */

/** Slots to bytes. The callers keep capacities inside the bound checked in _index_grow. */
static uint32_t index_bytes(uint32_t capacity) {
  return (uint32_t)((size_t)capacity * sizeof(void *));
}

void *hostmem_bvec_raw_alloc(uint32_t size, hostmem *allocator) {
  uint8_t *buffer = NULL;
  // hostmem_alloc rejects a zero size itself, so an empty request simply arrives back as NULL
  if (HOSTMEM_SUCCESS != hostmem_alloc(&buffer, size, allocator)) return NULL;
  return buffer;
}

bool hostmem_bvec_raw_free(void *ptr, uint32_t size, hostmem *allocator) {
  if (!ptr) return true;
  // strict on purpose: a warning means the arena kept the block, which _shrink must notice
  return HOSTMEM_SUCCESS == hostmem_free((uint8_t *)ptr, size, allocator);
}

bool hostmem_bvec_index_grow(
    void ***index, uint32_t old_capacity, uint32_t new_capacity, hostmem *allocator
) {
  if (!index || !new_capacity) return false;
  // slot counts are uint32_t, the byte size they stand for need not be; this is the gate that
  // decides it, and old_capacity passed through it when it was granted
  if (new_capacity > UINT32_MAX / sizeof(void *)) return false;
  // the warning counts as done here: an arena that had to move the block still resized it
  hostmem_result result = hostmem_realloc(
      (uint8_t **)index, index_bytes(old_capacity), index_bytes(new_capacity), allocator
  );
  return HOSTMEM_SUCCESS == result || HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED == result;
}

void hostmem_bvec_index_free(void **index, uint32_t capacity, hostmem *allocator) {
  hostmem_bvec_raw_free(index, index_bytes(capacity), allocator);
}
