#include "hostmem/memory.h"
#include "hostmem/multi_arena.h"
#include "hostmem/result.h"

#include "memory_limit.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <set>
#include <vector>

// Small arenas on purpose: every test crosses an arena boundary. The capacity stays well above
// HOSTMEM_MULTI_ARENA_FULL_REMAINING (128), or an arena would count as full the moment it opens
// and each allocation would get one of its own.
namespace {

constexpr uint32_t kArenaCapacity = 1024;

/** Every pointer the chain hands out is 8 byte aligned, in every arena. */
void ExpectAligned(const uint8_t *p) {
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);
}

/** Figures of a chain, or a zeroed set when the call fails — keeps the tests to one line. */
hostmem_multi_arena_stats Measure(const hostmem_multi_arena *m) {
  hostmem_multi_arena_stats stats{};
  EXPECT_EQ(hostmem_multi_arena_measure(m, &stats), HOSTMEM_SUCCESS);
  return stats;
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle and empty states
// ---------------------------------------------------------------------------

TEST(MultiArena, EmptyAfterInit) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 0u);
  const hostmem_multi_arena_stats stats = Measure(&m);
  EXPECT_EQ(stats.reserved, 0u);
  EXPECT_EQ(stats.used, 0u);
  EXPECT_EQ(stats.arena_count, 0u);
  EXPECT_EQ(stats.open_count, 0u);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, ZeroInitializedIsUsable) {
  // no init call at all: the empty state is all zeroes, and the default capacity applies
  hostmem_multi_arena m{};
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 32, &m), HOSTMEM_SUCCESS);
  ASSERT_NE(buffer, nullptr);
  ExpectAligned(buffer);

  const hostmem_multi_arena_stats stats = Measure(&m);
  EXPECT_EQ(stats.arena_count, 1u);
  EXPECT_EQ(stats.reserved, HOSTMEM_MULTI_ARENA_DEFAULT_CAPACITY);
  EXPECT_EQ(stats.used, 32u);

  hostmem_multi_arena_release(&m);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 0u);
}

TEST(MultiArena, InitRejectsBadArguments) {
  EXPECT_EQ(hostmem_multi_arena_init(nullptr, kArenaCapacity, nullptr), HOSTMEM_ERROR_NULL_POINTER);

  // rounding the capacity up to 8 would wrap uint32_t
  hostmem_multi_arena m{};
  EXPECT_EQ(hostmem_multi_arena_init(&m, UINT32_MAX, nullptr), HOSTMEM_ERROR_ARITHMETIC_OVERFLOW);
  EXPECT_EQ(hostmem_multi_arena_create(UINT32_MAX, nullptr), nullptr);
}

TEST(MultiArena, NullAllocatorIsNotAFallback) {
  // hostmem reads a NULL allocator as malloc/free; a NULL chain is a mistake
  uint8_t *buffer = nullptr;
  EXPECT_EQ(hostmem_multi_arena_alloc(&buffer, 8, nullptr), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(buffer, nullptr);
  EXPECT_EQ(hostmem_multi_arena_free(nullptr, 8, nullptr), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_multi_arena_reserve(nullptr, 4), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_multi_arena_shrink(nullptr), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_multi_arena_adopt(nullptr, nullptr, 64), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_multi_arena_measure(nullptr, nullptr), HOSTMEM_ERROR_NULL_POINTER);
  // NULL is a no-op, not a crash
  hostmem_multi_arena_reset(nullptr);
  hostmem_multi_arena_release(nullptr);
  hostmem_multi_arena_destroy(nullptr);
}

TEST(MultiArena, CreateAndDestroy) {
  hostmem_multi_arena *m = hostmem_multi_arena_create(kArenaCapacity, nullptr);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(hostmem_multi_arena_arena_count(m), 0u);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 64, m), HOSTMEM_SUCCESS);
  EXPECT_EQ(Measure(m).reserved, kArenaCapacity);

  hostmem_multi_arena_destroy(m);
}

// ---------------------------------------------------------------------------
// allocation
// ---------------------------------------------------------------------------

TEST(MultiArena, AllocRejectsBadArguments) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  EXPECT_EQ(hostmem_multi_arena_alloc(nullptr, 8, &m), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_multi_arena_alloc(&buffer, 0, &m), HOSTMEM_ERROR_INVALID_PARAM);
  EXPECT_EQ(hostmem_multi_arena_alloc(&buffer, UINT32_MAX, &m), HOSTMEM_ERROR_ARITHMETIC_OVERFLOW);
  // nothing was opened on the way
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 0u);
  EXPECT_EQ(buffer, nullptr);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, SizesRoundUpToEight) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&first, 1, &m), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_multi_arena_alloc(&second, 1, &m), HOSTMEM_SUCCESS);
  // one byte asked for, eight reserved — twice
  EXPECT_EQ(second - first, 8);
  EXPECT_EQ(Measure(&m).used, 16u);
  ExpectAligned(first);
  ExpectAligned(second);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, OpensAnotherArenaWhenTheCurrentOneIsFull) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  // 4 × 256 fills the first arena exactly
  std::vector<uint8_t *> blocks;
  for (int i = 0; i < 4; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 256, &m), HOSTMEM_SUCCESS);
    blocks.push_back(buffer);
  }
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 1u);
  EXPECT_EQ(Measure(&m).used, kArenaCapacity);

  uint8_t *fifth = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&fifth, 256, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  const hostmem_multi_arena_stats stats = Measure(&m);
  EXPECT_EQ(stats.reserved, 2u * kArenaCapacity);
  EXPECT_EQ(stats.used, kArenaCapacity + 256u);
  EXPECT_EQ(stats.open_count, 1u); // only the young one still has room

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, EveryBlockKeepsItsBytesAcrossManyArenas) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  constexpr uint32_t kBlockSize = 96;
  constexpr uint32_t kBlocks = 500; // far more than one arena holds
  std::vector<uint8_t *> blocks;
  std::set<const void *> distinct;

  for (uint32_t i = 0; i < kBlocks; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, kBlockSize, &m), HOSTMEM_SUCCESS) << i;
    ASSERT_NE(buffer, nullptr);
    ExpectAligned(buffer);
    ASSERT_TRUE(distinct.insert(buffer).second) << "block " << i << " handed out twice";
    std::memset(buffer, static_cast<int>(i & 0xFF), kBlockSize);
    blocks.push_back(buffer);
  }
  EXPECT_GT(hostmem_multi_arena_arena_count(&m), 1u);

  // the pattern survives every later allocation: an arena, once opened, never moves
  for (uint32_t i = 0; i < kBlocks; ++i) {
    for (uint32_t byte = 0; byte < kBlockSize; ++byte) {
      ASSERT_EQ(blocks[i][byte], static_cast<uint8_t>(i & 0xFF)) << "block " << i;
    }
  }

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, OversizedRequestGetsAnArenaOfItsOwn) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  uint8_t *small = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&small, 256, &m), HOSTMEM_SUCCESS);

  // four times what a regular arena holds: not refused, given ground of its own
  uint8_t *huge = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&huge, 4 * kArenaCapacity, &m), HOSTMEM_SUCCESS);
  ASSERT_NE(huge, nullptr);
  std::memset(huge, 0xAB, 4 * kArenaCapacity);

  hostmem_multi_arena_stats stats = Measure(&m);
  EXPECT_EQ(stats.arena_count, 2u);
  EXPECT_EQ(stats.reserved, 5u * kArenaCapacity);
  EXPECT_EQ(stats.open_count, 1u); // the dedicated arena is full on arrival

  // the first arena still has room, and the dedicated one does not get in the way
  uint8_t *another = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&another, 256, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(another, small + 256);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, AnArenaTooSmallForOneRequestStillServesTheNext) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  // leaves 512 bytes in arena 0 — above the full threshold, below the next request
  uint8_t *first = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&first, 512, &m), HOSTMEM_SUCCESS);

  uint8_t *big = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&big, 768, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  // arena 0 was skipped, not closed: a request it can hold lands there again
  uint8_t *small = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&small, 128, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(small, first + 512);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, AnArenaThatHasRunFullIsLeftBehindForGood) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  // 64 bytes left, at or below the full threshold: the front marker moves past this arena and
  // does not come back to it, even for a request its remainder would still hold
  uint8_t *first = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&first, 960, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(Measure(&m).open_count, 0u);

  uint8_t *tiny = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&tiny, 32, &m), HOSTMEM_SUCCESS);
  EXPECT_NE(tiny, first + 960);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, TheScanCarriesOnPastAnArenaThatIsTooSmall) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  // arena 0 keeps 224 bytes: too few for what follows, too many to count as full
  uint8_t *first = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&first, 800, &m), HOSTMEM_SUCCESS);
  uint8_t *second = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&second, 512, &m), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  // 400 fits neither arena 0 nor nothing: the scan steps over arena 0 and finds arena 1
  uint8_t *third = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&third, 400, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(third, second + 512);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, Clone) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  const uint8_t source[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  uint8_t *copy = nullptr;
  ASSERT_EQ(hostmem_multi_arena_clone(&copy, source, sizeof(source), &m), HOSTMEM_SUCCESS);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy, source);
  EXPECT_EQ(std::memcmp(copy, source, sizeof(source)), 0);
  // 9 bytes asked for, 16 reserved
  EXPECT_EQ(Measure(&m).used, 16u);

  EXPECT_EQ(hostmem_multi_arena_clone(nullptr, source, 4, &m), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_multi_arena_clone(&copy, nullptr, 4, &m), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_multi_arena_clone(&copy, source, 0, &m), HOSTMEM_ERROR_INVALID_PARAM);

  hostmem_multi_arena_release(&m);
}

// ---------------------------------------------------------------------------
// giving memory back
// ---------------------------------------------------------------------------

TEST(MultiArena, FreeTakesBackOnlyTheTailOfItsArena) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  uint8_t *first = nullptr;
  uint8_t *second = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&first, 64, &m), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_multi_arena_alloc(&second, 64, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(Measure(&m).used, 128u);

  // buried: the block stays where it is
  EXPECT_EQ(hostmem_multi_arena_free(first, 64, &m), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_EQ(Measure(&m).used, 128u);

  // the tail comes back, and the next request takes its place
  EXPECT_EQ(hostmem_multi_arena_free(second, 64, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(Measure(&m).used, 64u);
  uint8_t *again = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&again, 64, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(again, second);

  // NULL is never a tail
  EXPECT_EQ(hostmem_multi_arena_free(nullptr, 64, &m), HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, FreeRejectsAnAddressFromSomewhereElse) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 64, &m), HOSTMEM_SUCCESS);

  alignas(8) uint8_t foreign[64] = {0};
  EXPECT_EQ(hostmem_multi_arena_free(foreign, 64, &m), HOSTMEM_ERROR_INVALID_PARAM);
  EXPECT_EQ(Measure(&m).used, 64u);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, FreeReopensAnArenaThePathHadPassed) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  // fill arena 0 to the brim, then force arena 1 open
  uint8_t *last_in_first = nullptr;
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(hostmem_multi_arena_alloc(&last_in_first, 256, &m), HOSTMEM_SUCCESS);
  }
  uint8_t *in_second = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&in_second, 256, &m), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  // arena 0 has room again, and the front marker follows back to it
  ASSERT_EQ(hostmem_multi_arena_free(last_in_first, 256, &m), HOSTMEM_SUCCESS);
  uint8_t *reused = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&reused, 256, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(reused, last_in_first);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, ResetKeepsTheArenasAndAsksTheHostForNothing) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  uint8_t *very_first = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&very_first, 256, &m), HOSTMEM_SUCCESS);
  for (int i = 0; i < 10; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 256, &m), HOSTMEM_SUCCESS);
  }
  const uint32_t arenas = hostmem_multi_arena_arena_count(&m);
  ASSERT_GT(arenas, 1u);
  const uint64_t reserved = Measure(&m).reserved;

  hostmem_multi_arena_reset(&m);
  hostmem_multi_arena_stats stats = Measure(&m);
  EXPECT_EQ(stats.used, 0u);
  EXPECT_EQ(stats.arena_count, arenas);
  EXPECT_EQ(stats.reserved, reserved);
  EXPECT_EQ(stats.open_count, arenas);

  // the second pass runs inside the ground of the first
  uint8_t *after_reset = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&after_reset, 256, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(after_reset, very_first);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), arenas);

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, ShrinkReleasesTheTrailingEmptyArenas) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  for (int i = 0; i < 10; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 256, &m), HOSTMEM_SUCCESS);
  }
  ASSERT_GT(hostmem_multi_arena_arena_count(&m), 1u);

  // a shrink between allocations keeps everything: no trailing arena is empty
  ASSERT_EQ(hostmem_multi_arena_shrink(&m), HOSTMEM_SUCCESS);
  EXPECT_GT(hostmem_multi_arena_arena_count(&m), 1u);

  // after a reset the whole chain is empty and goes back to the host
  hostmem_multi_arena_reset(&m);
  ASSERT_EQ(hostmem_multi_arena_shrink(&m), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 0u);
  EXPECT_EQ(Measure(&m).reserved, 0u);

  // and the chain still works afterwards
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 64, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 1u);

  hostmem_multi_arena_release(&m);
}

// ---------------------------------------------------------------------------
// borrowed ground
// ---------------------------------------------------------------------------

TEST(MultiArena, AdoptedBufferIsUsedAndNeverFreed) {
  alignas(8) uint8_t host_block[512];
  std::memset(host_block, 0x5A, sizeof(host_block));

  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_multi_arena_adopt(&m, host_block, sizeof(host_block)), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 1u);
  EXPECT_EQ(Measure(&m).reserved, sizeof(host_block));

  // adopted before the first allocation, so it is filled first
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 128, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(buffer, host_block);

  hostmem_multi_arena_release(&m);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 0u);
  // the host's block was borrowed, not owned: still ours, still readable
  EXPECT_EQ(host_block[511], 0x5A);
}

TEST(MultiArena, ShrinkStopsAtBorrowedGround) {
  alignas(8) uint8_t host_block[512];

  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_multi_arena_adopt(&m, host_block, sizeof(host_block)), HOSTMEM_SUCCESS);

  // 512 bytes borrowed, then more than that asked for: an owned arena joins behind it
  for (int i = 0; i < 4; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 256, &m), HOSTMEM_SUCCESS);
  }
  ASSERT_EQ(hostmem_multi_arena_arena_count(&m), 2u);

  hostmem_multi_arena_reset(&m);
  ASSERT_EQ(hostmem_multi_arena_shrink(&m), HOSTMEM_SUCCESS);
  // the owned arena went back, the borrowed one stayed
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 1u);
  EXPECT_EQ(Measure(&m).reserved, sizeof(host_block));

  hostmem_multi_arena_release(&m);
}

TEST(MultiArena, AdoptRejectsBadArguments) {
  alignas(8) uint8_t host_block[64];

  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  EXPECT_EQ(hostmem_multi_arena_adopt(&m, nullptr, 64), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_multi_arena_adopt(&m, host_block, 0), HOSTMEM_ERROR_INVALID_PARAM);
  // capacity not a multiple of 8, and a base that is not 8 byte aligned
  EXPECT_EQ(hostmem_multi_arena_adopt(&m, host_block, 60), HOSTMEM_ERROR_INVALID_PARAM);
  EXPECT_EQ(hostmem_multi_arena_adopt(&m, host_block + 1, 56), HOSTMEM_ERROR_INVALID_PARAM);
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 0u);

  hostmem_multi_arena_release(&m);
}

// ---------------------------------------------------------------------------
// bookkeeping
// ---------------------------------------------------------------------------

TEST(MultiArena, BookkeepingCanComeFromAnArena) {
  // the descriptor vector draws from the host's blob as well; nothing calls malloc but the
  // arenas themselves
  alignas(8) uint8_t bookkeeping_block[4096];
  hostmem bookkeeping{};
  ASSERT_EQ(
      hostmem_init_arena_static(&bookkeeping, bookkeeping_block, sizeof(bookkeeping_block)),
      HOSTMEM_SUCCESS
  );

  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, &bookkeeping), HOSTMEM_SUCCESS);
  // an arena cannot reclaim a superseded index array, so the slots are taken once, up front
  ASSERT_EQ(hostmem_multi_arena_reserve(&m, 16), HOSTMEM_SUCCESS);
  const uint32_t after_reserve = bookkeeping.last_index;
  EXPECT_GT(after_reserve, 0u);

  for (int i = 0; i < 12; ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(hostmem_multi_arena_alloc(&buffer, 512, &m), HOSTMEM_SUCCESS);
  }
  EXPECT_GT(hostmem_multi_arena_arena_count(&m), 1u);
  // the reservation covered every descriptor: the bookkeeping arena never grew again
  EXPECT_EQ(bookkeeping.last_index, after_reserve);

  hostmem_multi_arena_release(&m);
  hostmem_release(&bookkeeping);
}

// ---------------------------------------------------------------------------
// exhaustion
// ---------------------------------------------------------------------------

#if defined(__linux__) && !defined(HOSTMEM_TEST_SKIP_MEMORY_LIMIT)
TEST(MultiArena, AnArenaThatCannotBeOpenedLeavesTheChainUntouched) {
  hostmem_multi_arena m;
  ASSERT_EQ(hostmem_multi_arena_init(&m, kArenaCapacity, nullptr), HOSTMEM_SUCCESS);

  uint8_t *kept = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&kept, 64, &m), HOSTMEM_SUCCESS);

  // beyond the address space this binary caps itself to (memory_limit.h)
  uint8_t *impossible = nullptr;
  EXPECT_EQ(
      hostmem_multi_arena_alloc(&impossible, UINT32_MAX - 7, &m), HOSTMEM_ERROR_OUT_OF_MEMORY
  );
  EXPECT_EQ(impossible, nullptr);

  // the failure changed nothing: same arena, same tail
  EXPECT_EQ(hostmem_multi_arena_arena_count(&m), 1u);
  EXPECT_EQ(Measure(&m).used, 64u);
  uint8_t *next = nullptr;
  ASSERT_EQ(hostmem_multi_arena_alloc(&next, 64, &m), HOSTMEM_SUCCESS);
  EXPECT_EQ(next, kept + 64);

  hostmem_multi_arena_release(&m);
}
#endif
