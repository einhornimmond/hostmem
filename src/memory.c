#include "hostmem/memory.h"
#include "hostmem/result.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Bump allocator: `last_index` walks forward through `data` and only walks back for the
 * block sitting right at it. Two invariants keep that cheap and every pointer 8 byte
 * aligned: `data` is aligned (malloc guarantees it, init_arena_borrow checks it), and
 * every size that moves the index goes through hostmem_align8_u32 first -- both directions.
 *
 * So no allocation needs padding, and alloc/free/realloc must agree on the *aligned*
 * size. A wrong old_size from a caller corrupts the arena. That rounding lives in memory.h
 * rather than here: the multi arena has to arrive at the same figure, and a second copy of
 * three lines is a second chance for the two to disagree.
 */

// Is this the block the bump index rests on? Only that one can be given back. Outside
// arena mode every block is owned individually, so always yes. Size must be aligned.
static bool is_reclaimable(const uint8_t *buffer, uint32_t aligned_size, const hostmem *memory) {
  if (!hostmem_is_arena(memory)) {
    return true;
  } else if (buffer && aligned_size) {
    return memory->data + memory->last_index - aligned_size == buffer;
  }
  return false;
}

// True if the request runs past the end. Records the shortfall for
// hostmem_overflow_total(), saturating -- a counter that rolls over to a small number
// is worse than one that is capped.
static bool account_capacity_exceeded(uint32_t aligned_size, hostmem *memory) {
  // no underflow: last_index never passes capacity
  if (memory->capacity - memory->last_index < aligned_size) {
    if ((uint64_t)memory->out_of_memory_capacity + (uint64_t)aligned_size > UINT32_MAX) {
      memory->out_of_memory_capacity = UINT32_MAX;
    } else {
      memory->out_of_memory_capacity += aligned_size;
    }
    return true;
  }
  return false;
}

// ********** manage memory allocator themself *******************

hostmem *hostmem_create(hostmem *allocator) {
  hostmem *memory = NULL;
  // the descriptor comes from wherever the caller points; NULL is malloc, as everywhere else
  if (HOSTMEM_SUCCESS != hostmem_alloc((uint8_t **)&memory, sizeof(hostmem), allocator)) {
    return NULL;
  }
  // zeroed, so it is in a valid state, even if using it with type = HOSTMEM_ALLOC_TYPE_DEFAULT the
  // same is as memory = NULL
  memset(memory, 0, sizeof(hostmem));
  return memory;
}

hostmem_result hostmem_init_arena(hostmem *memory, uint32_t capacity) {
  if (!memory) { return HOSTMEM_ERROR_NULL_POINTER; }
  if (!capacity) { return HOSTMEM_ERROR_INVALID_PARAM; }
  uint32_t aligned_capacity;
  if (!hostmem_align8_u32(capacity, &aligned_capacity)) {
    return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW;
  }

  // allocate before touching *memory, so a failure leaves it exactly as it was
  uint8_t *data = NULL;
  hostmem_result result = hostmem_alloc(&data, aligned_capacity, NULL);
  if (HOSTMEM_SUCCESS != result) { return result; }

  // every field is written, none is read: uninitialized storage is a valid input
  memory->data = data;
  memory->last_index = 0;
  memory->capacity = aligned_capacity;
  memory->out_of_memory_capacity = 0;
  memory->allocation_type = HOSTMEM_ALLOC_TYPE_ARENA_OWNED;
  return HOSTMEM_SUCCESS;
}

hostmem_result hostmem_init_arena_borrow(hostmem *memory, uint8_t *data, uint32_t capacity) {
  if (!memory || !data) { return HOSTMEM_ERROR_NULL_POINTER; }
  uint32_t aligned_capacity;
  if (!hostmem_align8_u32(capacity, &aligned_capacity)) {
    return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW;
  }
  // Rejected, not rounded: an unaligned base would break the "every pointer is 8 byte
  // aligned" invariant, and a rounded up capacity would let the arena bump past the end of
  // a buffer the caller sized exactly.
  if (!capacity || aligned_capacity != capacity ||
      HOSTMEM_ALIGN8((uintptr_t)data) != (uintptr_t)data) {
    return HOSTMEM_ERROR_INVALID_PARAM;
  }

  // like hostmem_init_arena: writes every field, reads none
  hostmem_reset(memory);
  memory->data = data;
  memory->capacity = aligned_capacity;
  memory->allocation_type = HOSTMEM_ALLOC_TYPE_ARENA_EXTERNAL;
  return HOSTMEM_SUCCESS;
}

void hostmem_release(hostmem *memory) {
  if (!memory) return;
  // external arenas belong to the caller, default mode holds nothing
  if (memory->data && HOSTMEM_ALLOC_TYPE_ARENA_OWNED == memory->allocation_type) {
    free(memory->data);
    memory->data = NULL;
  }
  memory->capacity = 0;
  hostmem_reset(memory);
}

hostmem_result hostmem_destroy(hostmem *memory, hostmem *allocator) {
  // nothing to give back is not a failure; hostmem_free would warn here, which would read as
  // "something stayed behind" when nothing was ever handed out
  if (!memory) { return HOSTMEM_SUCCESS; }
  hostmem_release(memory);
  // whatever the arena it was carved from answers is the caller's to see: the descriptor is
  // gone from their point of view either way, but its bytes may only come back on reset
  return hostmem_free((uint8_t *)memory, sizeof(hostmem), allocator);
}

// ********** manage memory allocations with data ptr and size explicit *******************

hostmem_result hostmem_alloc(uint8_t **buffer, uint32_t size, hostmem *memory) {
  if (!buffer) { return HOSTMEM_ERROR_NULL_POINTER; }
  if (!size) { return HOSTMEM_ERROR_INVALID_PARAM; }
  if (!hostmem_is_arena(memory)) {
    // through a local first: assigning malloc's result straight into *buffer would leave a NULL
    // behind on failure, and "failures leave every output untouched" has to hold here too
    uint8_t *allocated = (uint8_t *)malloc(size);
    if (!allocated) { return HOSTMEM_ERROR_OUT_OF_MEMORY; }
    *buffer = allocated;
    return HOSTMEM_SUCCESS;
  }
  // can only be happen, if caller access memory directly and mess with the state
  if (!memory->data) { return HOSTMEM_ERROR_INVALID_STATE; }

  // align with 8 Bytes
  uint32_t aligned_size;
  if (!hostmem_align8_u32(size, &aligned_size)) { return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW; }
  if (account_capacity_exceeded(aligned_size, memory)) { return HOSTMEM_ERROR_OUT_OF_MEMORY; }

  // last_index is already a multiple of 8, so no padding is needed here
  *buffer = memory->data + memory->last_index;
  memory->last_index += aligned_size;
  return HOSTMEM_SUCCESS;
}

hostmem_result hostmem_realloc(
    uint8_t **buffer, uint32_t old_size, uint32_t new_size, hostmem *memory
) {
  if (!buffer) { return HOSTMEM_ERROR_NULL_POINTER; }
  uint32_t new_size_aligned, old_size_aligned;
  if (!hostmem_align8_u32(new_size, &new_size_aligned)) {
    return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW;
  }
  if (!hostmem_align8_u32(old_size, &old_size_aligned)) {
    return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW;
  }

  // release on hostmem_free's terms and with its return value, so that freeing through here and
  // calling hostmem_free directly cannot drift apart. An empty buffer takes the same route.
  if (!new_size_aligned) {
    hostmem_result result = hostmem_free(*buffer, old_size_aligned, memory);
    if (HOSTMEM_SUCCESS == result) { *buffer = NULL; }
    return result;
  }

  // deliberately below the release check: (0, 0) means free, not "same size, nothing to do"
  if (*buffer && old_size == new_size) { return HOSTMEM_SUCCESS; }

  // realloc in non arena mode
  if (!hostmem_is_arena(memory)) {
    // realloc(NULL, n) is malloc(n), so a fresh buffer works here too
    uint8_t *resized = (uint8_t *)realloc(*buffer, new_size);
    if (!resized) { return HOSTMEM_ERROR_OUT_OF_MEMORY; }

    *buffer = resized;
    return HOSTMEM_SUCCESS;
  }
  // an arena can only resize in place at its tail
  if (is_reclaimable(*buffer, old_size_aligned, memory)) {
    // shrink: pull the bump index back over the bytes we no longer want
    if (new_size_aligned < old_size_aligned) {
      memory->last_index -= old_size_aligned - new_size_aligned;
      return HOSTMEM_SUCCESS;
    }

    // grow: nothing is allocated behind us, so we can just claim more
    uint32_t additional = new_size_aligned - old_size_aligned;
    if (account_capacity_exceeded(additional, memory)) { return HOSTMEM_ERROR_OUT_OF_MEMORY; }
    memory->last_index += additional;

    return HOSTMEM_SUCCESS;
  }

  // buried: growing has to take a fresh block and abandon the old one until reset
  if (new_size_aligned > old_size_aligned) {
    uint8_t *resized = NULL;
    hostmem_result result = hostmem_alloc(&resized, new_size_aligned, memory);
    if (HOSTMEM_SUCCESS != result) { return result; }

    if (*buffer && old_size) { memcpy(resized, *buffer, old_size); }
    *buffer = resized;
  }

  // Reached by both buried cases: the shrink did nothing, the grow above moved the buffer
  // and left bytes behind. Either way the resize is done and the memory is not back.
  return HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
}

hostmem_result hostmem_clone(
    uint8_t **dst_buffer, const uint8_t *src, uint32_t size, hostmem *memory
) {
  if (!dst_buffer || !src) { return HOSTMEM_ERROR_NULL_POINTER; }
  if (!size) { return HOSTMEM_ERROR_INVALID_PARAM; }

  hostmem_result result = hostmem_alloc(dst_buffer, size, memory);
  if (HOSTMEM_SUCCESS != result) { return result; }

  // copy the requested size, not what an arena reserved for it
  memcpy(*dst_buffer, src, size);
  return HOSTMEM_SUCCESS;
}

hostmem_result hostmem_free(uint8_t *buffer, uint32_t size, hostmem *memory) {
  if (!hostmem_is_arena(memory)) {
    free(buffer);
    return HOSTMEM_SUCCESS;
  }

  uint32_t aligned_size;
  if (!hostmem_align8_u32(size, &aligned_size)) { return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW; }
  if (is_reclaimable(buffer, aligned_size, memory)) {
    memory->last_index -= aligned_size;
    return HOSTMEM_SUCCESS;
  }

  // buried in the arena: the bytes come back on reset, not now
  return HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED;
}
