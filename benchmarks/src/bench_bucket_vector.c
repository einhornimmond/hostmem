#include "bench_report.h"
#include "hostmem/memory.h"
#include "hostmem/utils/bucket_vector.h"
#include "hostmem/utils/mono_timer.h"

#include <stdlib.h>
#include <string.h>

/*
 * What this benchmark measures
 *
 * A bucket vector trades one indirection on random access for two things a flat, doubling
 * array cannot give: appends that never copy what is already stored, and element addresses
 * that stay valid forever. The steps below put numbers on both sides of that trade —
 * append throughput, traversal, random access, and the cost of the bucket size itself.
 */

#define ELEMENT_COUNT 4000000

/** 64 byte payload — the size range where copying on growth really starts to hurt. */
typedef struct bench_payload {
  uint64_t id;
  uint64_t timestamp;
  uint8_t blob[48];
} bench_payload;

HOSTMEM_BVEC_STATIC(bvec_u64, uint64_t, 9)          /* 512 * 8 B = 4 KiB buckets */
HOSTMEM_BVEC_STATIC(bvec_u64_small, uint64_t, 5)    /* 32 * 8 B = 256 B buckets */
HOSTMEM_BVEC_STATIC(bvec_u64_large, uint64_t, 13)   /* 8192 * 8 B = 64 KiB buckets */
HOSTMEM_BVEC_STATIC(bvec_payload, bench_payload, 6) /* 64 * 64 B = 4 KiB buckets */

/** Prefilled sources for the read-side steps; filled once before the benchmarks run. */
static bvec_u64 g_filled;
static uint64_t *g_flat = NULL;
/** Kept across clear/refill so the step measures reuse of already allocated buckets. */
static bvec_u64 g_reused;
/** Arena large enough for ELEMENT_COUNT uint64 plus bucket index; reset per step. */
static hostmem g_arena;
/** Consumes every value read so the compiler cannot drop the traversal steps. */
static volatile uint64_t g_sink = 0;

/* --- append ----------------------------------------------------------------------------- */

static void test_bvec_push(int stepCount) {
  bvec_u64 v;
  bvec_u64_init(&v, NULL);
  for (int i = 0; i < stepCount; ++i) bvec_u64_push(&v, (uint64_t)i);
  bvec_u64_free(&v);
}

static void test_bvec_push_reserved(int stepCount) {
  bvec_u64 v;
  bvec_u64_init(&v, NULL);
  bvec_u64_reserve(&v, (uint32_t)stepCount);
  for (int i = 0; i < stepCount; ++i) bvec_u64_push(&v, (uint64_t)i);
  bvec_u64_free(&v);
}

static void test_bvec_push_arena(int stepCount) {
  bvec_u64 v;
  hostmem_reset(&g_arena);
  bvec_u64_init(&v, &g_arena);
  bvec_u64_reserve(&v, (uint32_t)stepCount);
  for (int i = 0; i < stepCount; ++i) bvec_u64_push(&v, (uint64_t)i);
  bvec_u64_free(&v);
}

/** The reference: a flat array that doubles and copies everything it already holds. */
static void test_flat_push(int stepCount) {
  uint64_t *flat = NULL;
  size_t capacity = 0, length = 0;
  for (int i = 0; i < stepCount; ++i) {
    if (length == capacity) {
      capacity = capacity ? capacity * 2 : 512;
      flat = (uint64_t *)realloc(flat, capacity * sizeof(uint64_t));
      if (!flat) return;
    }
    flat[length++] = (uint64_t)i;
  }
  free(flat);
}

/** Same appends, but into a vector whose buckets already exist — clear keeps them. */
static void test_bvec_refill_after_clear(int stepCount) {
  for (int i = 0; i < stepCount; ++i) bvec_u64_push(&g_reused, (uint64_t)i);
  bvec_u64_clear(&g_reused);
}

/* --- bucket size ------------------------------------------------------------------------ */

static void test_bvec_push_256b_buckets(int stepCount) {
  bvec_u64_small v;
  bvec_u64_small_init(&v, NULL);
  for (int i = 0; i < stepCount; ++i) bvec_u64_small_push(&v, (uint64_t)i);
  bvec_u64_small_free(&v);
}

static void test_bvec_push_64kb_buckets(int stepCount) {
  bvec_u64_large v;
  bvec_u64_large_init(&v, NULL);
  for (int i = 0; i < stepCount; ++i) bvec_u64_large_push(&v, (uint64_t)i);
  bvec_u64_large_free(&v);
}

/* --- payload ---------------------------------------------------------------------------- */

/** 64 byte payload passed by value: written twice, once to the stack, once into the bucket. */
static void test_payload_push_by_value(int stepCount) {
  bvec_payload v;
  bench_payload p;
  bvec_payload_init(&v, NULL);
  bvec_payload_reserve(&v, (uint32_t)stepCount);
  memset(&p, 0, sizeof(p));
  for (int i = 0; i < stepCount; ++i) {
    p.id = (uint64_t)i;
    bvec_payload_push(&v, p);
  }
  bvec_payload_free(&v);
}

/** The same payload built directly in its final slot — one write instead of two. */
static void test_payload_emplace(int stepCount) {
  bvec_payload v;
  bench_payload *slot;
  bvec_payload_init(&v, NULL);
  bvec_payload_reserve(&v, (uint32_t)stepCount);
  for (int i = 0; i < stepCount; ++i) {
    if (bvec_payload_emplace(&v, &slot) != HOSTMEM_SUCCESS) break;
    memset(slot, 0, sizeof(*slot));
    slot->id = (uint64_t)i;
  }
  bvec_payload_free(&v);
}

/* --- traversal and access --------------------------------------------------------------- */

/** Bucket by bucket: contiguous memory, no index lookup per element. */
static void test_bvec_iterate_buckets(int stepCount) {
  uint64_t sum = 0;
  (void)stepCount;
  for (uint32_t b = 0, buckets = bvec_u64_bucket_count(&g_filled); b < buckets; ++b) {
    const uint64_t *data = bvec_u64_bucket_data(&g_filled, b);
    const uint32_t count = bvec_u64_bucket_size(&g_filled, b);
    for (uint32_t k = 0; k < count; ++k) sum += data[k];
  }
  g_sink += sum;
}

/** Flat traversal through the FOREACH macro: one index lookup per element. */
static void test_bvec_iterate_foreach(int stepCount) {
  uint64_t sum = 0;
  uint64_t *item = NULL;
  (void)stepCount;
  HOSTMEM_BVEC_FOREACH(bvec_u64, &g_filled, item, index) {
    sum += *item;
  }
  g_sink += sum;
}

static void test_flat_iterate(int stepCount) {
  uint64_t sum = 0;
  for (int i = 0; i < stepCount; ++i) sum += g_flat[i];
  g_sink += sum;
}

/** Scattered reads — the case that pays for the extra indirection.
 *
 * The index runs in uint32_t here and in the flat reference alike: the modulo is the hottest
 * instruction in both loops, and a 64 bit divisor would put the difference in the step
 * instead of in the container. */
static void test_bvec_random_access(int stepCount) {
  uint64_t sum = 0;
  uint32_t index = 0;
  const uint32_t size = bvec_u64_size(&g_filled);
  for (int i = 0; i < stepCount; ++i) {
    index = (index + 524287) % size; /* prime stride: defeats the prefetcher */
    sum += *bvec_u64_get(&g_filled, index);
  }
  g_sink += sum;
}

static void test_flat_random_access(int stepCount) {
  uint64_t sum = 0;
  uint32_t index = 0;
  const uint32_t size = (uint32_t)ELEMENT_COUNT;
  for (int i = 0; i < stepCount; ++i) {
    index = (index + 524287) % size;
    sum += g_flat[index];
  }
  g_sink += sum;
}

/** Pointers taken before growth stay valid — this is what the indirection buys. */
static void test_bvec_push_pop_cycle(int stepCount) {
  bvec_u64 v;
  bvec_u64_init(&v, NULL);
  bvec_u64_reserve(&v, (uint32_t)stepCount);
  for (int i = 0; i < stepCount; ++i) bvec_u64_push(&v, (uint64_t)i);
  for (int i = 0; i < stepCount; ++i) bvec_u64_pop(&v);
  bvec_u64_free(&v);
}

/* --- driver ----------------------------------------------------------------------------- */

static void prepare_test_data(void) {
  g_flat = (uint64_t *)malloc((size_t)ELEMENT_COUNT * sizeof(uint64_t));
  bvec_u64_init(&g_filled, NULL);
  bvec_u64_reserve(&g_filled, (uint32_t)ELEMENT_COUNT);
  for (int i = 0; i < ELEMENT_COUNT; ++i) {
    bvec_u64_push(&g_filled, (uint64_t)i);
    g_flat[i] = (uint64_t)i;
  }
  /* filled once, then cleared: the refill step finds every bucket already in place */
  bvec_u64_init(&g_reused, NULL);
  bvec_u64_reserve(&g_reused, (uint32_t)ELEMENT_COUNT);
  bvec_u64_clear(&g_reused);
  /* payload for the index array included, so no step runs the arena dry */
  hostmem_init_arena(&g_arena, (uint32_t)ELEMENT_COUNT * sizeof(uint64_t) + 1024 * 1024);
}

static void release_test_data(void) {
  bvec_u64_free(&g_filled);
  bvec_u64_free(&g_reused);
  hostmem_release(&g_arena);
  free(g_flat);
}

int main(void) {
  hostmem_mono_timer timeUsed;
  const int stepCount = ELEMENT_COUNT;

  hostmem_mono_timer_init();
  hostmem_mono_timer_reset(&timeUsed);
  prepare_test_data();
  bench_prepared(timeUsed);

  bench_section("append");
  bench_step(test_bvec_push, stepCount, "  bucket vector push", "element");
  bench_step(test_bvec_push_reserved, stepCount, "  bucket vector push, reserved", "element");
  bench_step(test_bvec_push_arena, stepCount, "  bucket vector push, arena", "element");
  bench_step(
      test_bvec_refill_after_clear, stepCount, "  bucket vector refill after clear", "element"
  );
  bench_step(test_flat_push, stepCount, "  flat array push, doubling realloc", "element");
  bench_step(test_bvec_push_pop_cycle, stepCount, "  bucket vector push + pop cycle", "element");

  bench_section("bucket size (same 4 M appends)");
  bench_step(test_bvec_push_256b_buckets, stepCount, "  256 B buckets", "element");
  bench_step(test_bvec_push, stepCount, "  4 KiB buckets", "element");
  bench_step(test_bvec_push_64kb_buckets, stepCount, "  64 KiB buckets", "element");

  bench_section("64 byte payload");
  bench_step(test_payload_push_by_value, stepCount, "  push by value", "element");
  bench_step(test_payload_emplace, stepCount, "  emplace in place", "element");

  bench_section("traversal");
  bench_step(test_bvec_iterate_buckets, stepCount, "  bucket vector, bucket wise", "element");
  bench_step(test_bvec_iterate_foreach, stepCount, "  bucket vector, foreach", "element");
  bench_step(test_flat_iterate, stepCount, "  flat array", "element");

  bench_section("random access");
  bench_step(test_bvec_random_access, stepCount, "  bucket vector", "element");
  bench_step(test_flat_random_access, stepCount, "  flat array", "element");

  bench_total(timeUsed, stepCount, "element");

  release_test_data();
  return 0;
}
