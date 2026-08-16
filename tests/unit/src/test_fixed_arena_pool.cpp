#include "hostmem/fixed_arena_pool.h"
#include "hostmem/memory.h"
#include "hostmem/result.h"

#include "memory_limit.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <set>
#include <vector>

// Small arenas and few of them, so every test crosses the pool's ceiling rather than describing
// it. The capacity is a multiple of 8 already, so the rounding is not silently doing the work.
namespace {

constexpr uint32_t kArenaCapacity = 256;
constexpr uint16_t kArenaCount = 4;

/** Every pointer the pool hands out is 8 byte aligned, arenas and payload alike. */
void ExpectAligned(const void *p) {
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);
}

/** Take every arena the pool has, in order, and hand back the addresses. */
std::vector<hostmem *> DrainPool(hostmem_fixed_arena_pool *pool, uint16_t expected) {
  std::vector<hostmem *> taken;
  for (uint16_t i = 0; i < expected; ++i) {
    hostmem *arena = nullptr;
    EXPECT_EQ(hostmem_fixed_arena_pool_alloc(pool, &arena), HOSTMEM_SUCCESS) << "arena " << i;
    if (arena) { taken.push_back(arena); }
  }
  return taken;
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

TEST(FixedArenaPool, InitReservesEverythingUpFront) {
  hostmem_fixed_arena_pool pool;
  ASSERT_EQ(
      hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, kArenaCount, nullptr), HOSTMEM_SUCCESS
  );

  EXPECT_EQ(pool.arena_count, kArenaCount);
  EXPECT_EQ(pool.arena_capacity, kArenaCapacity);
  EXPECT_EQ(pool.acquired_count, 0u);
  EXPECT_EQ(hostmem_fixed_arena_pool_available(&pool), kArenaCount);

  // the whole footprint is known now and does not move afterwards
  const uint32_t reserved = hostmem_fixed_arena_pool_reserved(&pool);
  EXPECT_EQ(reserved, HOSTMEM_ALIGN8(kArenaCount * sizeof(hostmem)) + kArenaCount * kArenaCapacity);

  hostmem *arena = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &arena), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_reserved(&pool), reserved); // lending changes nothing
  ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, arena), HOSTMEM_SUCCESS);

  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_reserved(&pool), 0u);
}

TEST(FixedArenaPool, InitRejectsBadArguments) {
  hostmem_fixed_arena_pool pool{};
  EXPECT_EQ(
      hostmem_fixed_arena_pool_init(nullptr, kArenaCapacity, kArenaCount, nullptr),
      HOSTMEM_ERROR_NULL_POINTER
  );
  EXPECT_EQ(
      hostmem_fixed_arena_pool_init(&pool, 0, kArenaCount, nullptr), HOSTMEM_ERROR_INVALID_PARAM
  );
  EXPECT_EQ(
      hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, 0, nullptr), HOSTMEM_ERROR_INVALID_PARAM
  );
  // 65535 arenas of nearly 4 GiB each cannot be one block, and the guard says so before asking
  EXPECT_EQ(
      hostmem_fixed_arena_pool_init(&pool, HOSTMEM_MAX_ALLOC_SIZE, UINT16_MAX, nullptr),
      HOSTMEM_ERROR_ARITHMETIC_OVERFLOW
  );
  // the refusals left the descriptor exactly as it was
  EXPECT_EQ(pool.arena_count, 0u);
  EXPECT_EQ(pool.arenas, nullptr);
}

TEST(FixedArenaPool, CapacityRoundsUpToEight) {
  hostmem_fixed_arena_pool pool;
  ASSERT_EQ(hostmem_fixed_arena_pool_init(&pool, 100, 2, nullptr), HOSTMEM_SUCCESS);
  EXPECT_EQ(pool.arena_capacity, 104u); // and that is what the arenas really got

  hostmem *arena = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &arena), HOSTMEM_SUCCESS);
  EXPECT_EQ(arena->capacity, 104u);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 104, arena), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_alloc(&buffer, 1, arena), HOSTMEM_ERROR_OUT_OF_MEMORY);

  ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, arena), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
}

TEST(FixedArenaPool, CreateAndDestroy) {
  hostmem_fixed_arena_pool *pool =
      hostmem_fixed_arena_pool_create(kArenaCapacity, kArenaCount, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(hostmem_fixed_arena_pool_available(pool), kArenaCount);

  hostmem *arena = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(pool, &arena), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_fixed_arena_pool_free(pool, arena), HOSTMEM_SUCCESS);

  EXPECT_EQ(hostmem_fixed_arena_pool_destroy(pool, nullptr, nullptr), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_destroy(nullptr, nullptr, nullptr), HOSTMEM_SUCCESS);
}

TEST(FixedArenaPool, CreateRefusesArgumentsInitWouldRefuse) {
  EXPECT_EQ(hostmem_fixed_arena_pool_create(0, kArenaCount, nullptr, nullptr), nullptr);
  EXPECT_EQ(hostmem_fixed_arena_pool_create(kArenaCapacity, 0, nullptr, nullptr), nullptr);
}

// ---------------------------------------------------------------------------
// lending and returning
// ---------------------------------------------------------------------------

TEST(FixedArenaPool, EveryArenaIsDistinctAndUsable) {
  hostmem_fixed_arena_pool pool;
  ASSERT_EQ(
      hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, kArenaCount, nullptr), HOSTMEM_SUCCESS
  );

  const std::vector<hostmem *> taken = DrainPool(&pool, kArenaCount);
  ASSERT_EQ(taken.size(), kArenaCount);
  EXPECT_EQ(pool.acquired_count, kArenaCount);
  EXPECT_EQ(hostmem_fixed_arena_pool_available(&pool), 0u);

  // no arena is handed out twice, and none shares a byte with another
  std::set<const void *> descriptors;
  std::set<const void *> buffers;
  for (hostmem *arena : taken) {
    ExpectAligned(arena);
    ExpectAligned(arena->data);
    EXPECT_EQ(arena->capacity, kArenaCapacity);
    EXPECT_EQ(arena->last_index, 0u);
    EXPECT_TRUE(descriptors.insert(arena).second);
    EXPECT_TRUE(buffers.insert(arena->data).second);
  }

  // each fills to the brim without touching its neighbour: write a different byte in every
  // arena, then read them all back
  for (size_t i = 0; i < taken.size(); ++i) {
    uint8_t *buffer = nullptr;
    ASSERT_EQ(hostmem_alloc(&buffer, kArenaCapacity, taken[i]), HOSTMEM_SUCCESS);
    std::memset(buffer, static_cast<int>(0xA0 + i), kArenaCapacity);
  }
  for (size_t i = 0; i < taken.size(); ++i) {
    for (uint32_t b = 0; b < kArenaCapacity; ++b) {
      ASSERT_EQ(taken[i]->data[b], static_cast<uint8_t>(0xA0 + i))
          << "arena " << i << " byte " << b;
    }
  }

  for (hostmem *arena : taken) {
    ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, arena), HOSTMEM_SUCCESS);
  }
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
}

TEST(FixedArenaPool, AnExhaustedPoolSaysSoInsteadOfGrowing) {
  hostmem_fixed_arena_pool pool;
  ASSERT_EQ(
      hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, kArenaCount, nullptr), HOSTMEM_SUCCESS
  );
  const uint32_t reserved = hostmem_fixed_arena_pool_reserved(&pool);

  std::vector<hostmem *> taken = DrainPool(&pool, kArenaCount);
  ASSERT_EQ(taken.size(), kArenaCount);

  hostmem marker{};
  hostmem *out = &marker;
  EXPECT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &out), HOSTMEM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_EQ(out, &marker);                                       // a failure touches no output
  EXPECT_EQ(hostmem_fixed_arena_pool_reserved(&pool), reserved); // and asks the host for nothing

  // one back, one available again -- and it is the very arena that was returned
  hostmem *returned = taken.back();
  taken.pop_back();
  ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, returned), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_available(&pool), 1u);
  hostmem *again = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &again), HOSTMEM_SUCCESS);
  EXPECT_EQ(again, returned);

  taken.push_back(again);
  for (hostmem *arena : taken) {
    ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, arena), HOSTMEM_SUCCESS);
  }
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
}

TEST(FixedArenaPool, AReturnedArenaComesBackEmpty) {
  hostmem_fixed_arena_pool pool;
  ASSERT_EQ(hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, 1, nullptr), HOSTMEM_SUCCESS);

  hostmem *first = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &first), HOSTMEM_SUCCESS);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 64, first), HOSTMEM_SUCCESS);
  ASSERT_EQ(first->last_index, 64u);

  ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, first), HOSTMEM_SUCCESS);
  EXPECT_EQ(first->last_index, 0u); // reset happens on the way in, not on the way out

  hostmem *second = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &second), HOSTMEM_SUCCESS);
  EXPECT_EQ(second, first);
  EXPECT_EQ(second->last_index, 0u);
  uint8_t *again = nullptr;
  ASSERT_EQ(hostmem_alloc(&again, 64, second), HOSTMEM_SUCCESS);
  EXPECT_EQ(again, buffer); // the whole arena is available once more, from the front

  ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, second), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
}

TEST(FixedArenaPool, TheFreeListSurvivesReturnsInAnyOrder) {
  hostmem_fixed_arena_pool pool;
  ASSERT_EQ(
      hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, kArenaCount, nullptr), HOSTMEM_SUCCESS
  );

  // The list is threaded through the arenas themselves, so a return order that differs from the
  // lending order rewrites links in place. Every arena still has to come back exactly once.
  for (int round = 0; round < 50; ++round) {
    std::vector<hostmem *> taken = DrainPool(&pool, kArenaCount);
    ASSERT_EQ(taken.size(), kArenaCount) << "round " << round;

    // a different permutation each round, without a random source
    const size_t step = static_cast<size_t>(round % kArenaCount) + 1;
    std::vector<hostmem *> order;
    for (size_t i = 0, at = 0; i < taken.size(); ++i, at = (at + step) % taken.size()) {
      while (std::find(order.begin(), order.end(), taken[at]) != order.end()) {
        at = (at + 1) % taken.size();
      }
      order.push_back(taken[at]);
    }
    ASSERT_EQ(order.size(), kArenaCount);

    for (hostmem *arena : order) {
      ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, arena), HOSTMEM_SUCCESS) << "round " << round;
    }
    ASSERT_EQ(hostmem_fixed_arena_pool_available(&pool), kArenaCount) << "round " << round;
  }

  // and after all that churn the pool still holds exactly its arenas, no more and no fewer
  const std::vector<hostmem *> final_take = DrainPool(&pool, kArenaCount);
  const std::set<hostmem *> seen(final_take.begin(), final_take.end());
  EXPECT_EQ(seen.size(), kArenaCount) << "an arena went missing or came out twice";

  hostmem *none = nullptr;
  EXPECT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &none), HOSTMEM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_EQ(none, nullptr);

  for (hostmem *arena : final_take) {
    ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, arena), HOSTMEM_SUCCESS);
  }
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
}

TEST(FixedArenaPool, FreeRejectsWhatDidNotComeFromHere) {
  hostmem_fixed_arena_pool pool;
  hostmem_fixed_arena_pool other;
  ASSERT_EQ(
      hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, kArenaCount, nullptr), HOSTMEM_SUCCESS
  );
  ASSERT_EQ(
      hostmem_fixed_arena_pool_init(&other, kArenaCapacity, kArenaCount, nullptr), HOSTMEM_SUCCESS
  );

  hostmem *mine = nullptr;
  hostmem *theirs = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &mine), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(&other, &theirs), HOSTMEM_SUCCESS);

  EXPECT_EQ(hostmem_fixed_arena_pool_free(&pool, theirs), HOSTMEM_ERROR_INVALID_PARAM);
  EXPECT_EQ(pool.acquired_count, 1u); // the rejection changed nothing

  // an address inside the pool's array but not on a slot boundary is not an arena either
  hostmem *between = reinterpret_cast<hostmem *>(reinterpret_cast<uint8_t *>(pool.arenas) + 1);
  EXPECT_EQ(hostmem_fixed_arena_pool_free(&pool, between), HOSTMEM_ERROR_INVALID_PARAM);

  // a stack arena is not one of ours however well it is initialized
  alignas(8) uint8_t storage[64];
  hostmem stack_arena{};
  ASSERT_EQ(hostmem_init_arena_borrow(&stack_arena, storage, sizeof(storage)), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_free(&pool, &stack_arena), HOSTMEM_ERROR_INVALID_PARAM);

  ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, mine), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_fixed_arena_pool_free(&other, theirs), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&other, nullptr), HOSTMEM_SUCCESS);
  hostmem_release(&stack_arena);
}

TEST(FixedArenaPool, FreeCatchesTheReturnOfAnArenaNothingIsHolding) {
  hostmem_fixed_arena_pool pool;
  ASSERT_EQ(
      hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, kArenaCount, nullptr), HOSTMEM_SUCCESS
  );

  hostmem *arena = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &arena), HOSTMEM_SUCCESS);
  ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, arena), HOSTMEM_SUCCESS);

  // nothing is out, so this cannot be a return -- and letting it through would loop the list
  EXPECT_EQ(hostmem_fixed_arena_pool_free(&pool, arena), HOSTMEM_ERROR_INVALID_STATE);
  EXPECT_EQ(hostmem_fixed_arena_pool_available(&pool), kArenaCount);

  // the pool is unharmed: every arena still comes out exactly once
  std::set<const void *> seen;
  for (hostmem *taken : DrainPool(&pool, kArenaCount)) { EXPECT_TRUE(seen.insert(taken).second); }
  EXPECT_EQ(seen.size(), kArenaCount);

  for (const void *p : seen) {
    ASSERT_EQ(
        hostmem_fixed_arena_pool_free(
            &pool, const_cast<hostmem *>(static_cast<const hostmem *>(p))
        ),
        HOSTMEM_SUCCESS
    );
  }
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
}

TEST(FixedArenaPool, CallsOnAPoolThatOwnsNothing) {
  hostmem_fixed_arena_pool pool{};
  hostmem *arena = nullptr;

  // a zeroed pool holds nothing, and says that rather than reporting an empty free list
  EXPECT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &arena), HOSTMEM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(arena, nullptr);
  hostmem stack_arena{};
  EXPECT_EQ(hostmem_fixed_arena_pool_free(&pool, &stack_arena), HOSTMEM_ERROR_NOT_INITIALIZED);
  EXPECT_EQ(hostmem_fixed_arena_pool_available(&pool), 0u);
  EXPECT_EQ(hostmem_fixed_arena_pool_reserved(&pool), 0u);
  // releasing nothing is not a failure, and can be repeated
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);

  // NULL everywhere
  EXPECT_EQ(hostmem_fixed_arena_pool_alloc(nullptr, &arena), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_fixed_arena_pool_alloc(&pool, nullptr), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_fixed_arena_pool_free(nullptr, &stack_arena), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_fixed_arena_pool_free(&pool, nullptr), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_fixed_arena_pool_release(nullptr, nullptr), HOSTMEM_ERROR_NULL_POINTER);
  EXPECT_EQ(hostmem_fixed_arena_pool_available(nullptr), 0u);
  EXPECT_EQ(hostmem_fixed_arena_pool_reserved(nullptr), 0u);
}

// ---------------------------------------------------------------------------
// release and the memory behind it
// ---------------------------------------------------------------------------

TEST(FixedArenaPool, ReleaseRefusesWhileAnArenaIsStillOut) {
  hostmem_fixed_arena_pool pool;
  ASSERT_EQ(
      hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, kArenaCount, nullptr), HOSTMEM_SUCCESS
  );

  hostmem *arena = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(&pool, &arena), HOSTMEM_SUCCESS);
  uint8_t *buffer = nullptr;
  ASSERT_EQ(hostmem_alloc(&buffer, 32, arena), HOSTMEM_SUCCESS);
  std::memset(buffer, 0x5A, 32);

  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_ERROR_RESOURCE_IN_USE);
  // nothing was pulled away: the arena and its bytes are exactly where they were
  EXPECT_EQ(pool.acquired_count, 1u);
  EXPECT_NE(pool.arenas, nullptr);
  for (int i = 0; i < 32; ++i) { EXPECT_EQ(buffer[i], 0x5A); }
  EXPECT_EQ(arena->last_index, 32u);

  ASSERT_EQ(hostmem_fixed_arena_pool_free(&pool, arena), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, nullptr), HOSTMEM_SUCCESS);
}

TEST(FixedArenaPool, DestroyRefusesWhileAnArenaIsStillOutAndKeepsTheDescriptor) {
  hostmem_fixed_arena_pool *pool =
      hostmem_fixed_arena_pool_create(kArenaCapacity, kArenaCount, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);

  hostmem *arena = nullptr;
  ASSERT_EQ(hostmem_fixed_arena_pool_alloc(pool, &arena), HOSTMEM_SUCCESS);

  EXPECT_EQ(
      hostmem_fixed_arena_pool_destroy(pool, nullptr, nullptr), HOSTMEM_ERROR_RESOURCE_IN_USE
  );
  // the descriptor survived on purpose: the arena still has to be returned somewhere
  EXPECT_EQ(pool->acquired_count, 1u);
  ASSERT_EQ(hostmem_fixed_arena_pool_free(pool, arena), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_destroy(pool, nullptr, nullptr), HOSTMEM_SUCCESS);
}

TEST(FixedArenaPool, EverythingCanComeFromHostStorage) {
  // The shape a binding wants: descriptor, arenas and their buffers all inside blobs the host
  // owns, with nothing reaching malloc at any point.
  alignas(8) uint8_t descriptor_blob[256];
  alignas(8) uint8_t source_blob[4096];
  hostmem descriptor_store{};
  hostmem source_store{};
  ASSERT_EQ(
      hostmem_init_arena_borrow(&descriptor_store, descriptor_blob, sizeof(descriptor_blob)),
      HOSTMEM_SUCCESS
  );
  ASSERT_EQ(
      hostmem_init_arena_borrow(&source_store, source_blob, sizeof(source_blob)), HOSTMEM_SUCCESS
  );

  hostmem_fixed_arena_pool *pool = hostmem_fixed_arena_pool_create(
      kArenaCapacity, kArenaCount, &source_store, &descriptor_store
  );
  ASSERT_NE(pool, nullptr);

  // both blobs really carry it
  EXPECT_GE(reinterpret_cast<uintptr_t>(pool), reinterpret_cast<uintptr_t>(descriptor_blob));
  EXPECT_LT(
      reinterpret_cast<uintptr_t>(pool),
      reinterpret_cast<uintptr_t>(descriptor_blob) + sizeof(descriptor_blob)
  );
  EXPECT_EQ(source_store.last_index, hostmem_fixed_arena_pool_reserved(pool));

  const std::vector<hostmem *> taken = DrainPool(pool, kArenaCount);
  ASSERT_EQ(taken.size(), kArenaCount);
  for (hostmem *arena : taken) {
    EXPECT_GE(reinterpret_cast<uintptr_t>(arena->data), reinterpret_cast<uintptr_t>(source_blob));
    EXPECT_LE(
        reinterpret_cast<uintptr_t>(arena->data) + arena->capacity,
        reinterpret_cast<uintptr_t>(source_blob) + sizeof(source_blob)
    );
    ASSERT_EQ(hostmem_fixed_arena_pool_free(pool, arena), HOSTMEM_SUCCESS);
  }

  // the block is the tail of its source, so it comes back whole, and the descriptor after it
  EXPECT_EQ(
      hostmem_fixed_arena_pool_destroy(pool, &source_store, &descriptor_store), HOSTMEM_SUCCESS
  );
  EXPECT_EQ(source_store.last_index, 0u);
  EXPECT_EQ(descriptor_store.last_index, 0u);

  hostmem_release(&descriptor_store);
  hostmem_release(&source_store);
  source_blob[0] = 0x42; // the host's storage outlived all of it
  EXPECT_EQ(source_blob[0], 0x42);
}

TEST(FixedArenaPool, ASourceTooSmallIsRefusedWholeRatherThanInPart) {
  // "either every arena is there, or nothing is kept" -- checked against an arena that cannot
  // hold the block, where a per arena reservation would have stopped somewhere in the middle.
  alignas(8) uint8_t source_blob[512];
  hostmem source_store{};
  ASSERT_EQ(
      hostmem_init_arena_borrow(&source_store, source_blob, sizeof(source_blob)), HOSTMEM_SUCCESS
  );

  hostmem_fixed_arena_pool pool{};
  EXPECT_EQ(
      hostmem_fixed_arena_pool_init(&pool, kArenaCapacity, kArenaCount, &source_store),
      HOSTMEM_ERROR_OUT_OF_MEMORY
  );
  EXPECT_EQ(source_store.last_index, 0u); // not one arena was kept
  EXPECT_EQ(pool.arena_count, 0u);
  EXPECT_EQ(pool.arenas, nullptr);

  // and what does fit still works, from the very same source
  ASSERT_EQ(hostmem_fixed_arena_pool_init(&pool, 64, 2, &source_store), HOSTMEM_SUCCESS);
  EXPECT_EQ(hostmem_fixed_arena_pool_available(&pool), 2u);
  EXPECT_EQ(hostmem_fixed_arena_pool_release(&pool, &source_store), HOSTMEM_SUCCESS);
  EXPECT_EQ(source_store.last_index, 0u);

  hostmem_release(&source_store);
}

TEST(FixedArenaPool, ReleaseReportsWhatTheSourceCouldNotTakeBack) {
  alignas(8) uint8_t source_blob[4096];
  hostmem source_store{};
  ASSERT_EQ(
      hostmem_init_arena_borrow(&source_store, source_blob, sizeof(source_blob)), HOSTMEM_SUCCESS
  );

  hostmem_fixed_arena_pool pool;
  ASSERT_EQ(hostmem_fixed_arena_pool_init(&pool, 64, 2, &source_store), HOSTMEM_SUCCESS);
  uint8_t *on_top = nullptr;
  ASSERT_EQ(hostmem_alloc(&on_top, 32, &source_store), HOSTMEM_SUCCESS); // buries the block
  const uint32_t index_before = source_store.last_index;

  EXPECT_EQ(
      hostmem_fixed_arena_pool_release(&pool, &source_store),
      HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
  );
  // the pool let go regardless, and the source's index did not move by the wrong amount
  EXPECT_EQ(pool.arenas, nullptr);
  EXPECT_EQ(pool.arena_count, 0u);
  EXPECT_EQ(source_store.last_index, index_before);

  hostmem_release(&source_store);
}
