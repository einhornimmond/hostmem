#include "hostmem/memory.h"
#include "hostmem/memory_block.h"
#include "hostmem/result.h"
#include "memory_limit.h"
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <utility>

// The arena rounds every size up to a multiple of 8, so a request of `n` moves the
// bump index by HOSTMEM_ALIGN8(n). Tests that check `last_index` state that explicitly.

TEST(MemoryTest, AFailedAllocLeavesTheOutputPointerAlone) {
  // "Failures leave every output untouched", checked where every rejection is decided by the
  // arguments alone. Seeded with a real address rather than nullptr, so a written NULL shows up
  // instead of hiding behind the value the pointer already had.
  uint8_t marker = 0;
  uint8_t *buffer = &marker;

  alignas(8) uint8_t storage[64];
  hostmem arena{};
  ASSERT_EQ(hostmem_init_arena_borrow(&arena, storage, sizeof(storage)), HOSTMEM_SUCCESS);

  EXPECT_EQ(hostmem_alloc(&buffer, 0, &arena), HOSTMEM_ERROR_INVALID_PARAM);
  EXPECT_EQ(buffer, &marker);
  EXPECT_EQ(hostmem_alloc(&buffer, 0, nullptr), HOSTMEM_ERROR_INVALID_PARAM);
  EXPECT_EQ(buffer, &marker);
  // rounding up to 8 would wrap, so the arena refuses before reserving anything. Arena mode
  // only: the default path hands the size to the host untouched and has nothing to round.
  EXPECT_EQ(
      hostmem_alloc(&buffer, HOSTMEM_MAX_ALLOC_SIZE + 1, &arena), HOSTMEM_ERROR_ARITHMETIC_OVERFLOW
  );
  EXPECT_EQ(buffer, &marker);
  // a full arena, which needs no help from the host to say no
  EXPECT_EQ(hostmem_alloc(&buffer, 128, &arena), HOSTMEM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(buffer, &marker);

  // and a call that succeeds does replace it, so the checks above cannot pass by accident
  ASSERT_EQ(hostmem_alloc(&buffer, 32, &arena), HOSTMEM_SUCCESS);
  EXPECT_NE(buffer, &marker);
  hostmem_release(&arena);
}

TEST(MemoryTest, AFailedMallocLeavesTheOutputPointerAlone) {
  // The default path is where the promise is easy to lose: assigning malloc's result straight
  // into *buffer writes a NULL over whatever the caller held. Only a real refusal exercises it,
  // and only the address space cap can promise one — without it Linux overcommit answers a
  // 4 GiB request with an address, and this test would fail while leaking the block.
  constexpr unsigned long long kUnservable = HOSTMEM_MAX_ALLOC_SIZE;
  if (!HostmemTestAllocationMustFail(kUnservable)) {
    GTEST_SKIP() << "no address space cap in force, so no allocation can be made to fail";
  }

  uint8_t marker = 0;
  uint8_t *buffer = &marker;
  const hostmem_result result = hostmem_alloc(&buffer, HOSTMEM_MAX_ALLOC_SIZE, nullptr);

  // released before anything else, so an unexpected success cannot leave the block behind
  if (HOSTMEM_SUCCESS == result) { hostmem_free(buffer, HOSTMEM_MAX_ALLOC_SIZE, nullptr); }

  EXPECT_EQ(result, HOSTMEM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(buffer, &marker);
}

TEST(MemoryTest, Align8RoundsUpAndRefusesToWrap) {
  uint32_t aligned = 0xDEADBEEF;

  // 0 is allowed and stays 0; hostmem_alloc rejects it on its own terms, not here
  ASSERT_TRUE(hostmem_align8_u32(0, &aligned));
  EXPECT_EQ(aligned, 0u);

  for (const auto &c : {std::pair<uint32_t, uint32_t>{1, 8}, {3, 8}, {8, 8}, {9, 16}, {99, 104}}) {
    ASSERT_TRUE(hostmem_align8_u32(c.first, &aligned)) << c.first;
    EXPECT_EQ(aligned, c.second) << c.first;
  }

  // the ceiling is a multiple of 8 already, so it survives the rounding untouched
  ASSERT_TRUE(hostmem_align8_u32(HOSTMEM_MAX_ALLOC_SIZE, &aligned));
  EXPECT_EQ(aligned, HOSTMEM_MAX_ALLOC_SIZE);

  // one byte more cannot be rounded without wrapping, and the output stays as it was
  aligned = 0xDEADBEEF;
  EXPECT_FALSE(hostmem_align8_u32(HOSTMEM_MAX_ALLOC_SIZE + 1, &aligned));
  EXPECT_FALSE(hostmem_align8_u32(UINT32_MAX, &aligned));
  EXPECT_EQ(aligned, 0xDEADBEEFu);
}

TEST(MemoryTest, Align8IsTheFigureTheArenaActuallyReserves) {
  // The reason this rounding is shared rather than copied: what it returns has to be what the
  // bump index moves by, or freeing with the caller's size would move it back by the wrong
  // amount. Checked against the arena instead of against the formula.
  alignas(8) uint8_t storage[256];
  hostmem arena{};
  ASSERT_EQ(hostmem_init_arena_borrow(&arena, storage, sizeof(storage)), HOSTMEM_SUCCESS);

  for (uint32_t request : {1u, 7u, 8u, 9u, 31u, 32u}) {
    uint32_t expected = 0;
    ASSERT_TRUE(hostmem_align8_u32(request, &expected));

    const uint32_t before = arena.last_index;
    uint8_t *block = nullptr;
    ASSERT_EQ(hostmem_alloc(&block, request, &arena), HOSTMEM_SUCCESS) << request;
    EXPECT_EQ(arena.last_index - before, expected) << request;

    // and back again with the size the caller passed, which is where the two must agree
    ASSERT_EQ(hostmem_free(block, request, &arena), HOSTMEM_SUCCESS) << request;
    EXPECT_EQ(arena.last_index, before) << request;
  }

  hostmem_release(&arena);
}

TEST(MemoryTest, DynamicAreaAllocation) {
  // init
  hostmem mem{};
  EXPECT_EQ(hostmem_init_arena(&mem, 100), HOSTMEM_SUCCESS);
  // capacity is rounded up to a multiple of 8
  EXPECT_EQ(mem.capacity, 104u);

  // test valid alloc
  hostmem_memory_block block{};
  EXPECT_EQ(hostmem_memory_block_alloc(&block, 99, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(block.size, 99u);
  EXPECT_TRUE(block.data);
  // 99 bytes asked for, 104 reserved
  EXPECT_EQ(mem.last_index, 104u);

  // test alloc over the allocated area
  EXPECT_EQ(hostmem_memory_block_alloc(&block, 2, &mem), HOSTMEM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(hostmem_overflow_total(&mem), 8u);

  hostmem_release(&mem);
}

// ---------------------------------------------------------------------------
// allocator lifecycle
// ---------------------------------------------------------------------------

TEST(MemoryTest, InitArenaRejectsBadArguments) {
  hostmem mem{};
  EXPECT_EQ(hostmem_init_arena(nullptr, 64), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_init_arena(&mem, 0), HOSTMEM_ERROR_INVALID_PARAM);
  // rounding up to 8 would wrap uint32_t
  EXPECT_EQ(hostmem_init_arena(&mem, UINT32_MAX), HOSTMEM_ERROR_ARITHMETIC_OVERFLOW);
}

TEST(MemoryTest, InitArenaBorrowRejectsWhatItCannotHonour) {
  alignas(8) uint8_t storage[64];
  hostmem mem{};

  EXPECT_EQ(hostmem_init_arena_borrow(&mem, nullptr, 64), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_init_arena_borrow(&mem, storage, 0), HOSTMEM_ERROR_INVALID_PARAM);
  // an unaligned base would break the "every pointer is 8 byte aligned" invariant
  EXPECT_EQ(hostmem_init_arena_borrow(&mem, storage + 1, 32), HOSTMEM_ERROR_INVALID_PARAM);
  // and a capacity that is not a multiple of 8 is refused rather than rounded up: rounding
  // would let the arena hand out bytes past the end of a buffer the caller sized exactly
  for (uint32_t bad : {1u, 7u, 33u, 63u}) {
    EXPECT_EQ(hostmem_init_arena_borrow(&mem, storage, bad), HOSTMEM_ERROR_INVALID_PARAM)
        << "capacity " << bad;
  }

  ASSERT_EQ(hostmem_init_arena_borrow(&mem, storage, 64), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.allocation_type, HOSTMEM_ALLOC_TYPE_ARENA_EXTERNAL);
  EXPECT_EQ(mem.capacity, 64u);

  // the arena stays inside what it was given, right up to the last byte
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 64, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(buffer, storage);
  EXPECT_EQ(mem.last_index, mem.capacity);
  EXPECT_EQ(hostmem_alloc(&buffer, 1, &mem), HOSTMEM_ERROR_OUT_OF_MEMORY);

  // an external buffer belongs to the caller and survives the allocator
  hostmem_release(&mem);
  storage[0] = 0x42;
  EXPECT_EQ(storage[0], 0x42);
}

TEST(MemoryTest, InitArenaBorrowCanBeRepeatedWithoutFreeing) {
  // nothing is owned, so switching external buffers is just another init
  alignas(8) uint8_t first[64];
  alignas(8) uint8_t second[128];
  hostmem mem{};

  ASSERT_EQ(hostmem_init_arena_borrow(&mem, first, sizeof(first)), HOSTMEM_SUCCESS);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 32, &mem), HOSTMEM_SUCCESS);

  ASSERT_EQ(hostmem_init_arena_borrow(&mem, second, sizeof(second)), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.capacity, 128u);
  EXPECT_EQ(mem.last_index, 0u);
  ASSERT_EQ(hostmem_alloc(&buffer, 128, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(buffer, second);

  hostmem_release(&mem);
}

TEST(MemoryTest, InitArenaDoesNotReadPriorState) {
  // The point of splitting init and reinit: `hostmem mem;` followed by an init is the
  // most natural line to write, and it has to be correct. This emulates the stack garbage
  // that used to make init free a pointer it never owned.
  alignas(8) uint8_t not_from_malloc[64];
  hostmem mem;
  memset(&mem, 0xCD, sizeof(mem));
  mem.allocation_type = HOSTMEM_ALLOC_TYPE_ARENA_OWNED; // looks like a live owned arena
  mem.data = not_from_malloc;                           // but this was never malloc'd
  mem.capacity = sizeof(not_from_malloc);

  ASSERT_EQ(hostmem_init_arena(&mem, 128), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.capacity, 128u);
  EXPECT_EQ(mem.last_index, 0u);
  EXPECT_EQ(mem.out_of_memory_capacity, 0u);
  EXPECT_NE(mem.data, not_from_malloc);

  uint8_t *buffer = nullptr;
  EXPECT_EQ(hostmem_alloc(&buffer, 128, &mem), HOSTMEM_SUCCESS);
  hostmem_release(&mem);
}

TEST(MemoryTest, InitArenaLeavesTheAllocatorAloneOnFailure) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 64), HOSTMEM_SUCCESS);
  uint8_t *before = mem.data;

  // the allocation happens before anything is written, so a rejected request changes nothing
  EXPECT_EQ(hostmem_init_arena(&mem, UINT32_MAX), HOSTMEM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(mem.data, before);
  EXPECT_EQ(mem.capacity, 64u);

  uint8_t *buffer = nullptr;
  EXPECT_EQ(hostmem_alloc(&buffer, 64, &mem), HOSTMEM_SUCCESS);
  hostmem_release(&mem);
}

TEST(MemoryTest, ReinitArenaReplacesTheOwnedBuffer) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 64), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 64, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(mem.last_index, 64u);

  // releases the old arena and starts over: no leak, no double free
  ASSERT_EQ(hostmem_reinit_arena(&mem, 128), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.capacity, 128u);
  EXPECT_EQ(mem.last_index, 0u);

  ASSERT_EQ(hostmem_alloc(&buffer, 128, &mem), HOSTMEM_SUCCESS);
  hostmem_release(&mem);
}

TEST(MemoryTest, ReinitArenaWorksOnAZeroedAllocator) {
  // the free half has nothing to do, so reinit doubles as a plain init on a zeroed struct
  hostmem mem{};
  ASSERT_EQ(hostmem_reinit_arena(&mem, 64), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.capacity, 64u);
  EXPECT_EQ(mem.allocation_type, HOSTMEM_ALLOC_TYPE_ARENA_OWNED);

  uint8_t *buffer = nullptr;
  EXPECT_EQ(hostmem_alloc(&buffer, 64, &mem), HOSTMEM_SUCCESS);
  hostmem_release(&mem);
}

TEST(MemoryTest, CreateAndDestroy) {
  hostmem *mem = hostmem_create();
  ASSERT_TRUE(mem);
  // fresh from create it is in default mode: malloc/free
  EXPECT_EQ(mem->allocation_type, HOSTMEM_ALLOC_TYPE_DEFAULT);
  EXPECT_EQ(mem->capacity, 0u);
  EXPECT_EQ(mem->last_index, 0u);

  ASSERT_EQ(hostmem_init_arena(mem, 64), HOSTMEM_SUCCESS);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 16, mem), HOSTMEM_SUCCESS);

  // destroy releases the arena and the allocator itself
  hostmem_destroy(mem);
  hostmem_destroy(nullptr); // tolerated
}

TEST(MemoryTest, ResetDropsEverythingAtOnce) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 64), HOSTMEM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(hostmem_alloc(&first, 32, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_alloc(&second, 32, &mem), HOSTMEM_SUCCESS);
  // overflow the arena so the counter is non zero
  uint8_t *third = nullptr;
  ASSERT_EQ(hostmem_alloc(&third, 32, &mem), HOSTMEM_ERROR_OUT_OF_MEMORY);
  ASSERT_EQ(hostmem_overflow_total(&mem), 32u);

  hostmem_reset(&mem);
  EXPECT_EQ(mem.last_index, 0u);
  EXPECT_EQ(hostmem_overflow_total(&mem), 0u);

  // the arena buffer is kept, so the first allocation lands where it did before
  uint8_t *again = nullptr;
  ASSERT_EQ(hostmem_alloc(&again, 32, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(again, first);

  hostmem_reset(nullptr); // tolerated
  hostmem_release(&mem);
}

TEST(MemoryTest, OverflowCounterSaturatesInsteadOfWrapping) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 64), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  // each of these overshoots by nearly the whole uint32_t range
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(hostmem_alloc(&buffer, UINT32_MAX - 8, &mem), HOSTMEM_ERROR_OUT_OF_MEMORY);
  }
  // capped, not rolled over to a small number
  EXPECT_EQ(hostmem_overflow_total(&mem), UINT32_MAX);

  hostmem_release(&mem);
}

// ---------------------------------------------------------------------------
// hostmem_alloc
// ---------------------------------------------------------------------------

TEST(MemoryTest, AllocRejectsBadArguments) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 64), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  EXPECT_EQ(hostmem_alloc(nullptr, 8, &mem), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_alloc(&buffer, 0, &mem), HOSTMEM_ERROR_INVALID_PARAM);
  EXPECT_EQ(hostmem_alloc(&buffer, UINT32_MAX, &mem), HOSTMEM_ERROR_ARITHMETIC_OVERFLOW);
  // nothing of that touched the arena
  EXPECT_EQ(mem.last_index, 0u);

  hostmem_release(&mem);
}

TEST(MemoryTest, AllocOnUninitializedArenaReportsInvalidState) {
  // an arena type without a buffer can only come from writing the fields directly
  hostmem mem{};
  mem.allocation_type = HOSTMEM_ALLOC_TYPE_ARENA_OWNED;

  uint8_t *buffer = nullptr;
  EXPECT_EQ(hostmem_alloc(&buffer, 8, &mem), HOSTMEM_ERROR_INVALID_STATE);
}

TEST(MemoryTest, ArenaHandsOutEightByteAlignedPointers) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  // odd sizes, so only the internal rounding can keep the addresses aligned
  for (uint32_t size : {1u, 3u, 7u, 9u, 13u, 17u}) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(hostmem_alloc(&buffer, size, &mem), HOSTMEM_SUCCESS) << "size " << size;
    EXPECT_EQ((uintptr_t)buffer % 8, 0u) << "size " << size;
    EXPECT_EQ(mem.last_index % 8, 0u) << "size " << size;
  }

  hostmem_release(&mem);
}

TEST(MemoryTest, NullAllocatorMeansMalloc) {
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 32, nullptr), HOSTMEM_SUCCESS);
  ASSERT_TRUE(buffer);
  memset(buffer, 0x11, 32);

  ASSERT_EQ(hostmem_realloc(&buffer, 32, 64, nullptr), HOSTMEM_SUCCESS);
  ASSERT_TRUE(buffer);
  for (size_t i = 0; i < 32; ++i) { EXPECT_EQ(buffer[i], 0x11) << "at " << i; }

  EXPECT_EQ(hostmem_free(buffer, 64, nullptr), HOSTMEM_SUCCESS);
}

TEST(MemoryTest, DefaultModeBehavesLikeNullAllocator) {
  // a zeroed hostmem is default mode
  hostmem mem{};
  EXPECT_EQ(mem.allocation_type, HOSTMEM_ALLOC_TYPE_DEFAULT);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 16, &mem), HOSTMEM_SUCCESS);
  // default mode owns nothing collectively, so nothing is tracked
  EXPECT_EQ(mem.last_index, 0u);
  EXPECT_EQ(hostmem_overflow_total(&mem), 0u);

  EXPECT_EQ(hostmem_free(buffer, 16, &mem), HOSTMEM_SUCCESS);
  hostmem_release(&mem);
}

// ---------------------------------------------------------------------------
// hostmem_free
// ---------------------------------------------------------------------------

TEST(MemoryTest, FreeNothingIsHarmless) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 64), HOSTMEM_SUCCESS);

  // an empty buffer is not the arena's tail, so the arena reports it did not reclaim.
  EXPECT_EQ(hostmem_free(nullptr, 0, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(hostmem_free(nullptr, 32, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(mem.last_index, 0u);

  // outside arena mode free(NULL) is simply a no-op
  EXPECT_EQ(hostmem_free(nullptr, 32, nullptr), HOSTMEM_SUCCESS);

  hostmem_release(&mem);
}

TEST(MemoryTest, FreeReclaimsOnlyTheTail) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(hostmem_alloc(&first, 64, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_alloc(&second, 64, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(mem.last_index, 128u);

  // buried block: the bytes only come back on reset, and the call says so
  EXPECT_EQ(hostmem_free(first, 64, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(mem.last_index, 128u);

  // the tail comes back, and then the block before it is the new tail
  EXPECT_EQ(hostmem_free(second, 64, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 64u);
  EXPECT_EQ(hostmem_free(first, 64, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 0u);

  hostmem_release(&mem);
}

TEST(MemoryTest, FreeUnwindsUnalignedSizesExactly) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  // 13 -> 16 reserved, 5 -> 8 reserved; freeing in reverse must land back on 0
  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(hostmem_alloc(&first, 13, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_alloc(&second, 5, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(mem.last_index, 24u);

  EXPECT_EQ(hostmem_free(second, 5, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 16u);
  EXPECT_EQ(hostmem_free(first, 13, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 0u);

  hostmem_release(&mem);
}

// ---------------------------------------------------------------------------
// hostmem_realloc
// ---------------------------------------------------------------------------

TEST(MemoryTest, ReallocRejectsBadArguments) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 128), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 16, &mem), HOSTMEM_SUCCESS);

  EXPECT_EQ(hostmem_realloc(nullptr, 16, 8, &mem), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_realloc(&buffer, 16, UINT32_MAX, &mem), HOSTMEM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(hostmem_realloc(&buffer, UINT32_MAX, 16, &mem), HOSTMEM_ERROR_ARITHMETIC_OVERFLOW);
  // none of that moved anything
  EXPECT_EQ(mem.last_index, 16u);

  // same size is a no-op success
  EXPECT_EQ(hostmem_realloc(&buffer, 16, 16, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 16u);

  hostmem_release(&mem);
}

TEST(MemoryTest, ReallocToZeroFreesAndClearsThePointer) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 128), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 32, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(mem.last_index, 32u);

  EXPECT_EQ(hostmem_realloc(&buffer, 32, 0, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(buffer, nullptr);
  EXPECT_EQ(mem.last_index, 0u);

  // releasing nothing answers on hostmem_free()'s terms, which in arena mode is the warning:
  // NULL is never the tail, so nothing was reclaimed. Same as calling hostmem_free() directly.
  EXPECT_EQ(hostmem_realloc(&buffer, 0, 0, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(hostmem_free(nullptr, 0, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(buffer, nullptr);

  hostmem_release(&mem);
}

TEST(MemoryTest, ReallocToZeroKeepsThePointerWhenNothingWasReleased) {
  // the release path answers on hostmem_free()'s terms: the pointer is only cleared when
  // the bytes really came back, so a buried block stays addressable and says so
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *tail = nullptr;
  ASSERT_EQ(hostmem_alloc(&first, 32, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS);
  uint8_t *before = first;

  EXPECT_EQ(hostmem_realloc(&first, 32, 0, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(first, before);
  EXPECT_EQ(mem.last_index, 64u);

  // once it is the tail the very same call releases it and clears the pointer
  ASSERT_EQ(hostmem_free(tail, 32, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_realloc(&first, 32, 0, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(first, nullptr);
  EXPECT_EQ(mem.last_index, 0u);

  hostmem_release(&mem);
}

TEST(MemoryTest, ReallocFromNullAllocates) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 128), HOSTMEM_SUCCESS);

  // an empty buffer is not the arena's tail, so this goes down the fresh-block path
  uint8_t *buffer = nullptr;
  EXPECT_EQ(hostmem_realloc(&buffer, 0, 32, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  ASSERT_TRUE(buffer);
  EXPECT_EQ(mem.last_index, 32u);

  uint8_t *heap = nullptr;
  EXPECT_EQ(hostmem_realloc(&heap, 0, 32, nullptr), HOSTMEM_SUCCESS);
  ASSERT_TRUE(heap);
  EXPECT_EQ(hostmem_free(heap, 32, nullptr), HOSTMEM_SUCCESS);

  hostmem_release(&mem);
}

TEST(MemoryTest, ReallocArenaTailShrinkReclaims) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 128, &mem), HOSTMEM_SUCCESS);
  memset(buffer, 0xAB, 128);

  ASSERT_EQ(hostmem_realloc(&buffer, 128, 32, &mem), HOSTMEM_SUCCESS);
  // the tail is bumped back and the block keeps its address and contents
  EXPECT_EQ(mem.last_index, 32u);
  EXPECT_EQ(buffer[31], 0xAB);

  // the released bytes are handed out again
  uint8_t *reused = nullptr;
  ASSERT_EQ(hostmem_alloc(&reused, 96, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(reused, buffer + 32);

  hostmem_release(&mem);
}

TEST(MemoryTest, ReallocArenaTailGrowsInPlace) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 32, &mem), HOSTMEM_SUCCESS);
  uint8_t *before = buffer;
  memset(buffer, 0xCD, 32);

  ASSERT_EQ(hostmem_realloc(&buffer, 32, 64, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(buffer, before);
  EXPECT_EQ(mem.last_index, 64u);
  EXPECT_EQ(buffer[31], 0xCD);

  hostmem_release(&mem);
}

TEST(MemoryTest, ReallocArenaGrowBeyondCapacityFails) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 64), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 32, &mem), HOSTMEM_SUCCESS);
  uint8_t *before = buffer;

  EXPECT_EQ(hostmem_realloc(&buffer, 32, 128, &mem), HOSTMEM_ERROR_OUT_OF_MEMORY);
  // the buffer is left untouched and stays usable at its old size
  EXPECT_EQ(buffer, before);
  EXPECT_EQ(mem.last_index, 32u);
  EXPECT_EQ(hostmem_overflow_total(&mem), 96u);

  hostmem_release(&mem);
}

TEST(MemoryTest, ReallocArenaNonTailGrowMovesAndWarns) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *tail = nullptr;
  ASSERT_EQ(hostmem_alloc(&first, 32, &mem), HOSTMEM_SUCCESS);
  memset(first, 0x5A, 32);
  ASSERT_EQ(hostmem_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS);

  uint8_t *before = first;
  // the resize happened, the abandoned block did not come back — that is the warning
  EXPECT_EQ(hostmem_realloc(&first, 32, 48, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_NE(first, before);
  for (size_t i = 0; i < 32; ++i) { EXPECT_EQ(first[i], 0x5A) << "at " << i; }
  // 32 + 32 abandoned + 48 rounded to 48
  EXPECT_EQ(mem.last_index, 112u);

  hostmem_release(&mem);
}

TEST(MemoryTest, ReallocArenaNonTailShrinkChangesNothing) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *tail = nullptr;
  ASSERT_EQ(hostmem_alloc(&first, 64, &mem), HOSTMEM_SUCCESS);
  memset(first, 0x77, 64);
  ASSERT_EQ(hostmem_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS);

  uint8_t *before = first;
  uint32_t used_before = mem.last_index;

  EXPECT_EQ(hostmem_realloc(&first, 64, 16, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  // a buried shrink cannot reclaim, so nothing at all moves
  EXPECT_EQ(first, before);
  EXPECT_EQ(mem.last_index, used_before);
  EXPECT_EQ(first[63], 0x77);

  hostmem_release(&mem);
}

TEST(MemoryTest, ReallocDefaultAllocatorResizes) {
  hostmem mem{};

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 16, &mem), HOSTMEM_SUCCESS);
  memset(buffer, 0x3C, 16);

  ASSERT_EQ(hostmem_realloc(&buffer, 16, 64, &mem), HOSTMEM_SUCCESS);
  ASSERT_TRUE(buffer);
  for (size_t i = 0; i < 16; ++i) { EXPECT_EQ(buffer[i], 0x3C) << "at " << i; }

  ASSERT_EQ(hostmem_realloc(&buffer, 64, 8, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(buffer[7], 0x3C);

  EXPECT_EQ(hostmem_free(buffer, 8, &mem), HOSTMEM_SUCCESS);
  hostmem_release(&mem);
}

// ---------------------------------------------------------------------------
// hostmem_clone
// ---------------------------------------------------------------------------

TEST(MemoryTest, CloneCopiesExactlyTheRequestedSize) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 128), HOSTMEM_SUCCESS);

  const uint8_t source[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  uint8_t *copy = nullptr;

  EXPECT_EQ(hostmem_clone(nullptr, source, 13, &mem), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_clone(&copy, nullptr, 13, &mem), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_clone(&copy, source, 0, &mem), HOSTMEM_ERROR_INVALID_PARAM);

  ASSERT_EQ(hostmem_clone(&copy, source, 13, &mem), HOSTMEM_SUCCESS);
  ASSERT_TRUE(copy);
  EXPECT_EQ(memcmp(copy, source, 13), 0);
  // 13 asked for, 16 reserved
  EXPECT_EQ(mem.last_index, 16u);

  hostmem_release(&mem);
}

// ---------------------------------------------------------------------------
// hostmem_memory_block wrappers
// ---------------------------------------------------------------------------

TEST(MemoryBlockTest, RejectsNullDescriptors) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 64), HOSTMEM_SUCCESS);

  hostmem_memory_block block{};
  EXPECT_EQ(hostmem_memory_block_alloc(nullptr, 8, &mem), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_memory_block_realloc(nullptr, 8, &mem), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_memory_block_free(nullptr, &mem), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_memory_block_clone(nullptr, &block, &mem), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_memory_block_clone(&block, nullptr, &mem), HOSTMEM_ERROR_NULL_POINTER);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, FreeLeavesTheEmptyState) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 128), HOSTMEM_SUCCESS);

  hostmem_memory_block block{};
  ASSERT_EQ(hostmem_memory_block_alloc(&block, 32, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_memory_block_free(&block, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(block.data, nullptr);
  EXPECT_EQ(block.size, 0u);

  // freeing an already empty block changes nothing; the arena reports it reclaimed nothing,
  // which is true, and the descriptor stays in the empty state
  EXPECT_EQ(hostmem_memory_block_free(&block, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(block.data, nullptr);
  EXPECT_EQ(block.size, 0u);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, FreeKeepsTheDescriptorWhenTheArenaKeepsTheBytes) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  hostmem_memory_block first{};
  hostmem_memory_block tail{};
  ASSERT_EQ(hostmem_memory_block_alloc(&first, 32, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_memory_block_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS);
  uint8_t *before = first.data;

  // the descriptor is only reset when the bytes really came back; a buried block keeps
  // pointing at memory that is still valid until the arena resets
  EXPECT_EQ(hostmem_memory_block_free(&first, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(first.data, before);
  EXPECT_EQ(first.size, 32u);
  EXPECT_EQ(mem.last_index, 64u);

  // once the block above it is gone, the same call reclaims and does reset
  ASSERT_EQ(hostmem_memory_block_free(&tail, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_memory_block_free(&first, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(first.data, nullptr);
  EXPECT_EQ(first.size, 0u);
  EXPECT_EQ(mem.last_index, 0u);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, FreeAndReallocToZeroAgreeOnABuriedBlock) {
  // Both spellings mean "release this block" and must leave the same descriptor behind:
  // a block the arena would not take back keeps both its pointer and its size. A half
  // cleared {data, size: 0} could never be reclaimed afterwards, because a size of 0
  // never matches the arena tail.
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  hostmem_memory_block viaFree{};
  hostmem_memory_block viaRealloc{};
  hostmem_memory_block tail{};
  ASSERT_EQ(hostmem_memory_block_alloc(&viaFree, 32, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_memory_block_alloc(&viaRealloc, 32, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_memory_block_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS);
  uint8_t *freeBefore = viaFree.data;
  uint8_t *reallocBefore = viaRealloc.data;

  EXPECT_EQ(hostmem_memory_block_free(&viaFree, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(viaFree.data, freeBefore);
  EXPECT_EQ(viaFree.size, 32u);

  EXPECT_EQ(
      hostmem_memory_block_realloc(&viaRealloc, 0, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
  );
  EXPECT_EQ(viaRealloc.data, reallocBefore);
  EXPECT_EQ(viaRealloc.size, 32u);

  // neither gave anything back to the arena
  EXPECT_EQ(mem.last_index, 96u);

  // and because both descriptors are intact, the blocks unwind properly once they are the tail
  ASSERT_EQ(hostmem_memory_block_free(&tail, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_memory_block_realloc(&viaRealloc, 0, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(viaRealloc.data, nullptr);
  EXPECT_EQ(viaRealloc.size, 0u);
  EXPECT_EQ(hostmem_memory_block_free(&viaFree, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 0u);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, ReallocKeepsSizeAndPointerInStep) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  hostmem_memory_block block{};
  ASSERT_EQ(hostmem_memory_block_alloc(&block, 128, &mem), HOSTMEM_SUCCESS);
  memset(block.data, 0xAB, block.size);
  uint8_t *before = block.data;

  ASSERT_EQ(hostmem_memory_block_realloc(&block, 32, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(block.size, 32u);
  EXPECT_EQ(block.data, before);
  EXPECT_EQ(mem.last_index, 32u);

  // and the block is still consistent enough to free itself completely
  EXPECT_EQ(hostmem_memory_block_free(&block, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 0u);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, ReallocRecordsTheNewSizeOnTheArenaWarning) {
  // the regression this guards: a warning is not a failure, so the descriptor has to
  // follow the resize — otherwise data points at the new block and size at the old one
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  hostmem_memory_block first{};
  hostmem_memory_block tail{};
  ASSERT_EQ(hostmem_memory_block_alloc(&first, 32, &mem), HOSTMEM_SUCCESS);
  memset(first.data, 0x5A, first.size);
  ASSERT_EQ(hostmem_memory_block_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS);

  uint8_t *before = first.data;
  EXPECT_EQ(
      hostmem_memory_block_realloc(&first, 48, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
  );
  // the block could not grow in place, so it moved — and the recorded size has to move with
  // it, or the caller cannot use the space it just paid for
  EXPECT_NE(first.data, before);
  EXPECT_EQ(first.size, 48u);
  for (size_t i = 0; i < 32; ++i) { EXPECT_EQ(first.data[i], 0x5A) << "at " << i; }

  // the descriptor is right, so freeing it as the tail actually reclaims
  EXPECT_EQ(hostmem_memory_block_free(&first, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 64u);

  hostmem_release(&mem);
}

// The two buried-block resizes are where `size` earns its definition: it records what the
// block was allocated with, because that is the number hostmem_free() is told later. Both tests
// check the descriptor *and* then prove it by reclaiming — a descriptor that disagrees with
// the arena silently strands the block forever.

TEST(MemoryBlockTest, ReallocToZeroIsInterchangeableWithFree) {
  // Sweeps every allocator state a release can start from and requires the two spellings to
  // agree on all of it: return value, descriptor, and what the arena took back. The empty
  // arena case is the subtle one — NULL is never the tail, so both must report the warning.
  enum Scenario { ARENA_TAIL, ARENA_BURIED, ARENA_EMPTY, HEAP_BLOCK, HEAP_EMPTY };
  const char *names[] = {"arena tail", "arena buried", "arena empty", "heap block", "heap empty"};

  for (int k = ARENA_TAIL; k <= HEAP_EMPTY; ++k) {
    hostmem_result result[2];
    bool cleared[2];
    uint32_t size[2];
    uint32_t last_index[2];

    for (int op = 0; op < 2; ++op) { // 0 = _free, 1 = _realloc(0)
      hostmem mem{};
      const bool arena = k <= ARENA_EMPTY;
      if (arena) { ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS) << names[k]; }

      hostmem_memory_block victim{};
      hostmem_memory_block tail{};
      if (k == ARENA_TAIL || k == ARENA_BURIED || k == HEAP_BLOCK) {
        ASSERT_EQ(hostmem_memory_block_alloc(&victim, 32, &mem), HOSTMEM_SUCCESS) << names[k];
      }
      if (k == ARENA_BURIED) {
        ASSERT_EQ(hostmem_memory_block_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS) << names[k];
      }

      result[op] = op == 0 ? hostmem_memory_block_free(&victim, &mem)
                           : hostmem_memory_block_realloc(&victim, 0, &mem);
      cleared[op] = victim.data == nullptr;
      size[op] = victim.size;
      last_index[op] = mem.last_index;

      if (arena) {
        hostmem_release(&mem);
      } else if (victim.data) {
        EXPECT_EQ(hostmem_memory_block_free(&victim, &mem), HOSTMEM_SUCCESS);
      }
    }

    EXPECT_EQ(result[0], result[1]) << names[k];
    EXPECT_EQ(cleared[0], cleared[1]) << names[k];
    EXPECT_EQ(size[0], size[1]) << names[k];
    EXPECT_EQ(last_index[0], last_index[1]) << names[k];
  }
}

TEST(MemoryBlockTest, ReallocBuriedGrowKeepsTheBlockReclaimable) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  hostmem_memory_block victim{};
  hostmem_memory_block tail{};
  ASSERT_EQ(hostmem_memory_block_alloc(&victim, 32, &mem), HOSTMEM_SUCCESS);
  memset(victim.data, 0xA5, victim.size);
  ASSERT_EQ(hostmem_memory_block_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS);
  uint8_t *before = victim.data;

  // buried, so the arena cannot extend in place: it takes a fresh 48 byte block and copies
  EXPECT_EQ(
      hostmem_memory_block_realloc(&victim, 48, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
  );
  EXPECT_NE(victim.data, before);
  EXPECT_EQ(victim.size, 48u);
  for (size_t i = 0; i < 32; ++i) { EXPECT_EQ(victim.data[i], 0xA5) << "at " << i; }
  // 32 abandoned + 32 tail + 48 new
  EXPECT_EQ(mem.last_index, 112u);

  // the caller really owns 48 usable bytes now, not the 32 it started with
  memset(victim.data, 0x3C, victim.size);
  EXPECT_EQ(victim.data[47], 0x3C);

  // and because size followed the move, the new block is reclaimable once it is the tail
  ASSERT_EQ(hostmem_memory_block_free(&tail, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(hostmem_memory_block_free(&victim, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 64u);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, ReallocBuriedShrinkKeepsTheRecordedSize) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  hostmem_memory_block victim{};
  hostmem_memory_block tail{};
  ASSERT_EQ(hostmem_memory_block_alloc(&victim, 64, &mem), HOSTMEM_SUCCESS);
  memset(victim.data, 0x77, victim.size);
  ASSERT_EQ(hostmem_memory_block_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS);
  uint8_t *before = victim.data;

  // a buried shrink cannot reclaim, so literally nothing happens — and the descriptor must
  // keep the size the arena reserved, not the smaller one the caller asked for
  EXPECT_EQ(
      hostmem_memory_block_realloc(&victim, 16, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
  );
  EXPECT_EQ(victim.data, before);
  EXPECT_EQ(victim.size, 64u);
  EXPECT_EQ(mem.last_index, 96u);
  EXPECT_EQ(victim.data[63], 0x77);

  // recording 16 here would have made the block permanently unreclaimable, because a size
  // that does not match the reservation never matches the arena tail
  ASSERT_EQ(hostmem_memory_block_free(&tail, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_memory_block_free(&victim, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 0u);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, ReallocBuriedShrinkThenRegrowStaysConsistent) {
  // the follow-up the recorded size protects: after a refused shrink the block is still 64,
  // so a later resize hands hostmem_realloc the right old size
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  hostmem_memory_block victim{};
  hostmem_memory_block tail{};
  ASSERT_EQ(hostmem_memory_block_alloc(&victim, 64, &mem), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_memory_block_alloc(&tail, 32, &mem), HOSTMEM_SUCCESS);

  ASSERT_EQ(
      hostmem_memory_block_realloc(&victim, 16, &mem), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
  );
  // free the tail: victim is the tail again, and still 64 bytes wide
  ASSERT_EQ(hostmem_memory_block_free(&tail, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(mem.last_index, 64u);

  // now the shrink can be honoured in place, exactly by the 48 bytes it should be
  EXPECT_EQ(hostmem_memory_block_realloc(&victim, 16, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(victim.size, 16u);
  EXPECT_EQ(mem.last_index, 16u);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, ReallocToZeroReleasesTheBlock) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 128), HOSTMEM_SUCCESS);

  hostmem_memory_block block{};
  ASSERT_EQ(hostmem_memory_block_alloc(&block, 32, &mem), HOSTMEM_SUCCESS);

  EXPECT_EQ(hostmem_memory_block_realloc(&block, 0, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(block.data, nullptr);
  EXPECT_EQ(block.size, 0u);
  EXPECT_EQ(mem.last_index, 0u);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, ReallocLeavesTheDescriptorAloneOnFailure) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 64), HOSTMEM_SUCCESS);

  hostmem_memory_block block{};
  ASSERT_EQ(hostmem_memory_block_alloc(&block, 32, &mem), HOSTMEM_SUCCESS);
  uint8_t *before = block.data;

  EXPECT_EQ(hostmem_memory_block_realloc(&block, 128, &mem), HOSTMEM_ERROR_OUT_OF_MEMORY);
  // still usable at its previous size
  EXPECT_EQ(block.size, 32u);
  EXPECT_EQ(block.data, before);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, CloneCopiesContentAndSize) {
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 256), HOSTMEM_SUCCESS);

  hostmem_memory_block source{};
  ASSERT_EQ(hostmem_memory_block_alloc(&source, 13, &mem), HOSTMEM_SUCCESS);
  memset(source.data, 0x2B, source.size);

  hostmem_memory_block copy{};
  ASSERT_EQ(hostmem_memory_block_clone(&copy, &source, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(copy.size, source.size);
  EXPECT_NE(copy.data, source.data);
  EXPECT_EQ(memcmp(copy.data, source.data, source.size), 0);

  // a source and destination on different allocators is fine too
  hostmem_memory_block heap_copy{};
  ASSERT_EQ(hostmem_memory_block_clone(&heap_copy, &source, nullptr), HOSTMEM_SUCCESS);
  EXPECT_EQ(memcmp(heap_copy.data, source.data, source.size), 0);
  EXPECT_EQ(hostmem_memory_block_free(&heap_copy, nullptr), HOSTMEM_SUCCESS);

  hostmem_release(&mem);
}

TEST(MemoryBlockTest, CloneLeavesDestinationAloneOnFailure) {
  // the descriptor must not claim a size it never got
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena(&mem, 32), HOSTMEM_SUCCESS);

  uint8_t payload[64];
  memset(payload, 0x99, sizeof(payload));
  hostmem_memory_block source = {payload, sizeof(payload)};

  hostmem_memory_block copy{};
  EXPECT_EQ(hostmem_memory_block_clone(&copy, &source, &mem), HOSTMEM_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(copy.data, nullptr);
  EXPECT_EQ(copy.size, 0u);

  hostmem_release(&mem);
}

// ---------------------------------------------------------------------------
// the pattern the wire decoders use: hand back the unused tail of a scratch block
// ---------------------------------------------------------------------------

TEST(MemoryBlockTest, ReleasesUnusedScratchTail) {
  alignas(8) uint8_t storage[256];
  hostmem mem{};
  ASSERT_EQ(hostmem_init_arena_borrow(&mem, storage, sizeof(storage)), HOSTMEM_SUCCESS);

  hostmem_memory_block keep{};
  ASSERT_EQ(hostmem_memory_block_alloc(&keep, 16, &mem), HOSTMEM_SUCCESS);

  // take everything that is left as scratch space, like the decoders do for pbtools
  hostmem_memory_block scratch{};
  ASSERT_EQ(
      hostmem_memory_block_alloc(&scratch, mem.capacity - mem.last_index, &mem), HOSTMEM_SUCCESS
  );
  EXPECT_EQ(mem.last_index, mem.capacity);

  // only the first 40 bytes were actually used
  ASSERT_EQ(hostmem_memory_block_realloc(&scratch, 40, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(scratch.size, 40u);
  EXPECT_EQ(mem.last_index, 56u);

  // and the rest is available again
  hostmem_memory_block after{};
  EXPECT_EQ(hostmem_memory_block_alloc(&after, 128, &mem), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_overflow_total(&mem), 0u);

  hostmem_release(&mem);
}

// ---------------------------------------------------------------------------
// the guard around the guards
// ---------------------------------------------------------------------------

#if defined(__linux__) && !defined(HOSTMEM_TEST_SKIP_MEMORY_LIMIT)
#include <sys/resource.h>

TEST(TestMemoryLimit, IsInEffectForThisBinary) {
  // memory_limit.h caps this process so a runaway allocation dies here instead of taking the
  // machine down. If that ever stops working, everything else still passes and nobody notices
  // until the next 64 GB afternoon — so check it directly.
  const char *env = std::getenv("HOSTMEM_TEST_MEMORY_LIMIT_MB");
  if (env && std::string(env) == "0") { GTEST_SKIP() << "cap disabled via environment"; }

  rlimit limit{};
  ASSERT_EQ(getrlimit(RLIMIT_AS, &limit), 0);
  ASSERT_NE(limit.rlim_cur, RLIM_INFINITY) << "address space is uncapped";

  // and it actually bites: far more than any test legitimately needs
  void *huge = malloc(static_cast<size_t>(64) * 1024 * 1024 * 1024);
  EXPECT_EQ(huge, nullptr) << "a 64 GB request went through";
  free(huge);
}
#endif
