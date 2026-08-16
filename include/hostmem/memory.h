#ifndef HOSTMEM_MEMORY_H
#define HOSTMEM_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup hostmem_memory hostmem_memory
 *  @brief Allocator that is either a bump arena or plain malloc/free.
 *
 *  These rules hold for every function below:
 *
 *  - The allocator is the last argument and NULL is valid there: it means
 *    malloc/free, same as @ref HOSTMEM_ALLOC_TYPE_DEFAULT. A call site picks a
 *    strategy by what it passes, not by which function it calls.
 *  - Sizes are passed in, never stored, so freeing and resizing need the size the
 *    caller allocated with. A wrong size makes the arena move its index by the
 *    wrong amount. @ref hostmem_memory_block (utils/memory_block.h) keeps pointer and
 *    size together when that bookkeeping should not be the caller's job.
 *  - Every size rounds up to a multiple of 8, which keeps all returned pointers
 *    8 byte aligned. One that would wrap uint32_t yields HOSTMEM_ERROR_ARITHMETIC_OVERFLOW.
 *  - An arena can only give back its most recent allocation; anything before it stays
 *    reserved until @ref hostmem_reset. Calls that could not reclaim return
 *    @ref HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED -- the operation happened, the memory
 *    did not come back. Handle it explicitly; it is neither a failure nor a release.
 *  - Failures leave every output untouched.
 *
 *  @{
 */

/* Alignment: Most CPUs access memory more efficiently if data starts at
 * addresses that are multiples of 4 or 8. We'll align to 8 bytes.
 * This macro takes a size 'x' and rounds it UP to the nearest multiple of 8.
 * Example: HOSTMEM_ALIGN8(3) -> 8, HOSTMEM_ALIGN8(8) -> 8, HOSTMEM_ALIGN8(10) -> 16
 */
#define HOSTMEM_ALIGN8(x) (((x) + 7) & (~7))

/** @brief Largest size a single allocation may ask for.
 *
 * Every size rounds up to a multiple of 8, so the ceiling is the largest such multiple that
 * still fits uint32_t: rounding anything above it would wrap, and the allocator answers
 * HOSTMEM_ERROR_ARITHMETIC_OVERFLOW instead. Callers that derive a maximum element count from
 * a byte budget should divide this constant rather than UINT32_MAX, so their bound and the
 * allocator's agree.
 */
#define HOSTMEM_MAX_ALLOC_SIZE (UINT32_MAX - 7u)

/** @brief Round a size up to the multiple of 8 the allocator will actually reserve for it.
 *
 *  @ref HOSTMEM_ALIGN8 with the one check the bare macro cannot carry: a size above
 *  @ref HOSTMEM_MAX_ALLOC_SIZE would wrap on the way up, and wrapping here would hand back a
 *  number smaller than what was asked for. Every size that moves an arena's index passes through
 *  this, in both directions, so allocating and freeing agree on the aligned figure -- and a caller
 *  sizing a buffer ahead of time gets at the same number instead of reimplementing it.
 *
 *  @param[in]  size    Size to round up; 0 is allowed and yields 0.
 *  @param[out] aligned Receives the rounded size; not NULL. Untouched when the rounding wraps.
 *  @return true when @p aligned was written, false when @p size exceeds
 *          @ref HOSTMEM_MAX_ALLOC_SIZE -- the callers turn that into
 *          HOSTMEM_ERROR_ARITHMETIC_OVERFLOW.
 */
static inline bool hostmem_align8_u32(uint32_t size, uint32_t *aligned) {
  if (size > HOSTMEM_MAX_ALLOC_SIZE) { return false; }

  *aligned = HOSTMEM_ALIGN8(size);
  return true;
}

/** @brief Allocation strategy and ownership of the arena buffer. */
typedef enum hostmem_alloc_type {
  HOSTMEM_ALLOC_TYPE_DEFAULT = 0,   /**< Individual malloc/free per allocation. */
  HOSTMEM_ALLOC_TYPE_ARENA_OWNED,   /**< Bump allocator, buffer owned by the allocator. */
  HOSTMEM_ALLOC_TYPE_ARENA_EXTERNAL /**< Bump allocator, buffer owned by the caller. */
} hostmem_alloc_type;

/** @brief Memory allocator state container.
 *
 *  The two init functions write every field and read none, so they accept uninitialized
 *  storage -- @c hostmem mem; followed by an init is correct. Everything else needs an
 *  allocator that is either initialized or zeroed; @c hostmem mem = {0}; is the valid
 *  empty state and means default mode (malloc/free).
 *
 *  @note Do not write these fields; @p last_index and @p capacity are kept multiples of 8.
 */
typedef struct hostmem {
  uint8_t *data;                   /**< Base of the arena (owned or external), 8 byte aligned. */
  uint32_t last_index;             /**< Next free offset from @p data. */
  uint32_t capacity;               /**< Bytes available in the arena, rounded up to 8. */
  uint32_t out_of_memory_capacity; /**< Accumulated overflow since last reset, saturating. */
  uint8_t allocation_type;         /**< hostmem_alloc_type, one byte is enough. */
} hostmem;

// ********** manage memory allocator themself *******************

/** @brief Allocate and zero a hostmem on the heap, ready for an init call.
 *
 *  @return Zeroed allocator in default mode, or NULL when the heap is exhausted.
 *  @note Pair with hostmem_destroy().
 *  @whisper A vessel for vessels, itself drawn from the stream
 */
hostmem *hostmem_create();

/** @brief Initialize arena mode with an owned heap buffer.
 *
 *  Writes every field and reads none, so uninitialized storage is a valid input.
 *
 *  @param[in,out] memory   Allocator to initialize; not NULL. Need not be zeroed.
 *  @param[in]     capacity Bytes to reserve, rounded up to 8; must be > 0.
 *  @retval HOSTMEM_SUCCESS             Arena ready, bump index at 0.
 *  @retval HOSTMEM_ERROR_NULL_POINTER  @p memory is NULL.
 *  @retval HOSTMEM_ERROR_INVALID_PARAM @p capacity is 0.
 *  @retval HOSTMEM_ERROR_OUT_OF_MEMORY malloc failed; @p memory is left untouched.
 *  @warning Calling this on an allocator that already owns an arena leaks that buffer.
 *           Use hostmem_reinit_arena() to replace one.
 *  @whisper The basin is dug, and waits for water
 */
hostmem_result hostmem_init_arena(hostmem *memory, uint32_t capacity);

/** @brief Borrow a caller owned buffer and run arena mode inside it.
 *
 *  The buffer is filled but never freed: hostmem_release() lets @p data go untouched, so it
 *  stays the caller's throughout. Suited to stack or static storage that outlives the
 *  allocator. Like hostmem_init_arena() it writes every field and reads none.
 *  hostmem_multi_arena_borrow() borrows the same way, one arena at a time into a chain.
 *
 *  Both @p data and @p capacity must be multiples of 8 and are rejected otherwise, rather
 *  than rounded. Rounding a capacity up would let the arena bump past the end of a buffer
 *  the caller sized exactly -- seven bytes of silent corruption is not worth the convenience.
 *
 *  Re-borrowing over another borrowed buffer needs nothing else: there is no buffer to
 *  give back, so just call this again. Only an allocator that currently owns a *heap* arena
 *  has something to release first -- hostmem_release() it before switching to a borrowed
 *  buffer, or it leaks.
 *
 *  @param[in,out] memory   Allocator to initialize; not NULL. Need not be zeroed.
 *  @param[in]     data     Buffer to bump through; not NULL, 8 byte aligned (@c alignas(8)).
 *  @param[in]     capacity Usable bytes in @p data; must be > 0 and a multiple of 8.
 *  @retval HOSTMEM_SUCCESS             Arena ready, bump index at 0.
 *  @retval HOSTMEM_ERROR_NULL_POINTER  @p memory or @p data is NULL.
 *  @retval HOSTMEM_ERROR_INVALID_PARAM @p capacity is 0 or not a multiple of 8, or @p data is
 *                                  not 8 byte aligned.
 *  @whisper Borrowed ground, returned unbroken
 */
hostmem_result hostmem_init_arena_borrow(hostmem *memory, uint8_t *data, uint32_t capacity);

/** @brief Drop every outstanding allocation at once, keeping the buffer. O(1).
 *
 *  Moves the bump index back to 0 and clears the overflow counter. Nothing to do in
 *  default mode.
 *
 *  @param[in,out] memory Allocator to rewind; may be NULL.
 *  @warning Every pointer this arena handed out dangles afterwards.
 *  @whisper The tide goes out, the basin whole again
 */
static inline void hostmem_reset(hostmem *memory) {
  if (memory) {
    memory->last_index = 0;
    memory->out_of_memory_capacity = 0;
  }
}

/** @brief Release what the allocator owns, but not the allocator itself.
 *
 *  Frees the buffer in @ref HOSTMEM_ALLOC_TYPE_ARENA_OWNED mode and rewinds. External
 *  buffers stay untouched, default mode holds nothing. @p allocation_type is kept, so the
 *  allocator stays the kind it was.
 *
 *  @param[in,out] memory Allocator to empty; may be NULL.
 *  @whisper Waters recede; the basin returns to silence
 */
void hostmem_release(hostmem *memory);

/** @brief Replace the arena of an allocator that already has one.
 *
 *  hostmem_release() followed by hostmem_init_arena(): releases what is held, then
 *  reserves @p capacity fresh bytes. This is the call that resizes an arena.
 *
 *  Unlike the init functions this one reads @p memory, so it needs an allocator that was
 *  initialized before, or a zeroed one (where the free half simply does nothing).
 *
 *  @param[in,out] memory   Initialized or zeroed allocator; not NULL.
 *  @param[in]     capacity Bytes for the new arena, rounded up to 8; must be > 0.
 *  @return Whatever hostmem_init_arena() returns. On failure the old arena is already
 *          gone and @p memory is left empty in its previous mode.
 *  @warning Every pointer the old arena handed out dangles afterwards.
 *  @whisper The basin emptied, then dug anew
 */
static inline hostmem_result hostmem_reinit_arena(hostmem *memory, uint32_t capacity) {
  hostmem_release(memory);
  return hostmem_init_arena(memory, capacity);
}

/** @brief hostmem_release(), then release the hostmem itself.
 *
 *  @param[in] memory From hostmem_create(), never stack or static storage; may be NULL.
 *  @whisper The vessel that held vessels returns to the stream
 */
void hostmem_destroy(hostmem *memory);

/** @brief Bytes worth of requests that did not fit since the last reset.
 *
 *  Says how much larger the arena would have to be. Saturates at UINT32_MAX instead of
 *  wrapping, and is always 0 in default mode. hostmem_reset() clears it.
 *
 *  @param[in] memory Allocator to query; may be NULL.
 *  @return Total bytes of failed requests, or 0 if @p memory is NULL.
 *  @whisper The measure of need that exceeded the vessel
 */
size_t hostmem_overflow_total(const hostmem *memory);

// ********** manage memory allocations with data ptr and size explicit *******************

/** @brief Allocate a raw buffer: malloc, or a bump of the arena index.
 *
 *  @param[out]    buffer Receives the allocation; not NULL.
 *  @param[in]     size   Bytes to allocate; must be > 0. An arena reserves HOSTMEM_ALIGN8(size).
 *  @param[in,out] memory Allocator to draw from, or NULL for malloc.
 *  @retval HOSTMEM_SUCCESS             Buffer allocated.
 *  @retval HOSTMEM_ERROR_NULL_POINTER  @p buffer is NULL.
 *  @retval HOSTMEM_ERROR_INVALID_PARAM @p size is 0.
 *  @retval HOSTMEM_ERROR_INVALID_STATE Arena mode without a buffer: never initialized, or the
 *                                  fields were written directly.
 *  @retval HOSTMEM_ERROR_OUT_OF_MEMORY malloc failed, or the arena is full -- the shortfall goes
 *                                  to hostmem_overflow_total().
 *  @note The memory is not zeroed and holds whatever the previous tenant left.
 *  @whisper Raw earth shaped by the hand of need
 */
hostmem_result hostmem_alloc(uint8_t **buffer, uint32_t size, hostmem *memory);

/** @brief Resize a buffer, in place where the allocator allows it.
 *
 *  Outside arena mode this is realloc(): the block may move, contents are preserved. In
 *  arena mode only the tail block can touch the bump index, so:
 *
 *  | block    | direction | what happens                            | returns |
 *  |----------|-----------|-----------------------------------------|---------|
 *  | tail     | shrink    | index moves back, bytes reusable        | SUCCESS |
 *  | tail     | grow      | index moves on, address unchanged       | SUCCESS |
 *  | non tail | grow      | fresh block, @p old_size bytes copied   | WARNING |
 *  | non tail | shrink    | nothing at all, address and bytes kept  | WARNING |
 *
 *  @p new_size 0 releases the block through hostmem_free() and is interchangeable with it,
 *  down to the return value: @c *buffer is cleared only when the bytes really came back.
 *
 *  @param[in,out] buffer   Not NULL, but may point to NULL to allocate from scratch.
 *                          Updated when the block moves.
 *  @param[in]     old_size Size the buffer was allocated with; only the arena needs it, to
 *                          recognize the tail.
 *  @param[in]     new_size Requested size, or 0 to free.
 *  @param[in,out] memory   Allocator the buffer came from, or NULL for realloc.
 *  @retval HOSTMEM_SUCCESS            Resized, or the sizes were already equal.
 *  @retval HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Per the table. On @p new_size 0 it means
 *                                 the block was not released at all.
 *  @retval HOSTMEM_ERROR_NULL_POINTER @p buffer is NULL.
 *  @retval HOSTMEM_ERROR_OUT_OF_MEMORY realloc failed, or the arena has no room to grow.
 *  @whisper The vessel widens or narrows, the water within untouched
 */
hostmem_result hostmem_realloc(
    uint8_t **buffer, uint32_t old_size, uint32_t new_size, hostmem *memory
);

/** @brief hostmem_alloc() plus memcpy; copies exactly @p size bytes.
 *
 *  @param[out]    dst_buffer Receives the copy; not NULL.
 *  @param[in]     src        Source holding @p size bytes; not NULL.
 *  @param[in]     size       Bytes to copy; must be > 0.
 *  @param[in,out] memory     Allocator to draw from, or NULL for malloc.
 *  @retval HOSTMEM_SUCCESS             Copy allocated and filled.
 *  @retval HOSTMEM_ERROR_NULL_POINTER  @p dst_buffer or @p src is NULL.
 *  @retval HOSTMEM_ERROR_INVALID_PARAM @p size is 0.
 *  @retval Anything hostmem_alloc() can return.
 *  @whisper Water poured into a vessel newly shaped
 */
hostmem_result hostmem_clone(
    uint8_t **dst_buffer, const uint8_t *src, uint32_t size, hostmem *memory
);

/** @brief Free a buffer: free(), or move the arena index back if it is the tail.
 *
 *  @param[in]     buffer Buffer to release; may be NULL, which changes nothing -- though an
 *                        arena still warns, since NULL is never its tail.
 *  @param[in]     size   Size the buffer was allocated with. Ignored outside arena mode.
 *  @param[in,out] memory Allocator the buffer came from, or NULL for free().
 *  @retval HOSTMEM_SUCCESS Buffer freed, or the arena reclaimed its bytes.
 *  @retval HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Not the arena's last allocation, so the
 *                      block is still there -- do not treat it as released.
 *  @warning A @p size that does not match the allocation moves the index by the wrong
 *           amount and hands the same bytes out twice.
 *  @whisper Form dissolves, substance returning to source
 */
hostmem_result hostmem_free(uint8_t *buffer, uint32_t size, hostmem *memory);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // HOSTMEM_MEMORY_H
