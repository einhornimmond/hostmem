#include "bench_report.h"
#include "hostmem/converter.h"
#include "hostmem/duration.h"
#include "hostmem/mono_timer.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * What this benchmark measures
 *
 * Turning an integer into decimal digits is the conversion a host does most often, and the
 * obvious way — snprintf — parses a format string on every call. The steps below put that
 * baseline next to the hand written digit loop: once with the length worked out inside the
 * call, once with a length the caller already knew.
 */

#define TEST_VALUES_COUNT 1000
#define STRING_BUFFER_SIZE 32

static uint64_t testValues[TEST_VALUES_COUNT];
static uint8_t testSizes[TEST_VALUES_COUNT];
/** Receives every conversion, so the compiler cannot drop the call. */
static char benchBuffer[STRING_BUFFER_SIZE];

static int cursor = 0;

static uint64_t getNextTestValue(void) {
  uint64_t result = testValues[cursor++];
  if (cursor >= TEST_VALUES_COUNT) { cursor = 0; }
  return result;
}

/* --- unsigned ---------------------------------------------------------------------------- */

static void test_snprintf_uint64(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    snprintf(benchBuffer, STRING_BUFFER_SIZE, "%" PRIu64, getNextTestValue());
  }
}

static void test_uint64_to_string(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    hostmem_uint64_to_string(benchBuffer, STRING_BUFFER_SIZE, getNextTestValue());
  }
}

/** The same conversion for a caller that already knows the digit count. */
static void test_uint64_to_string_known_size(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    const int slot = cursor;
    hostmem_uint64_to_string_known_string_size(benchBuffer, getNextTestValue(), testSizes[slot]);
  }
}

/** Only the length, without writing a digit — what sizing a buffer costs on its own. */
static void test_uint64_to_string_size(int stepCount) {
  uint8_t sink = 0;
  for (int i = 0; i < stepCount; ++i) { sink ^= hostmem_uint64_to_string_size(getNextTestValue()); }
  benchBuffer[0] = (char)sink;
}

/* --- signed ------------------------------------------------------------------------------ */

static void test_snprintf_int64(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    snprintf(benchBuffer, STRING_BUFFER_SIZE, "%" PRId64, -(int64_t)(getNextTestValue() >> 1));
  }
}

static void test_int64_to_string(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    hostmem_int64_to_string(benchBuffer, STRING_BUFFER_SIZE, -(int64_t)(getNextTestValue() >> 1));
  }
}

/* --- duration ---------------------------------------------------------------------------- */

/** Nanoseconds to a readable span: the same digit loop, plus picking a unit for it. */
static void test_duration_to_string(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    hostmem_duration_string(
        benchBuffer, STRING_BUFFER_SIZE, (hostmem_duration)(getNextTestValue() >> 24), 4
    );
  }
}

/* --- driver ------------------------------------------------------------------------------ */

static void prepare_test_data(void) {
  srand(12812);
  for (int i = 0; i < TEST_VALUES_COUNT; ++i) {
    testValues[i] = ((uint64_t)rand() << 48) ^ ((uint64_t)rand() << 32) ^ ((uint64_t)rand() << 16) ^
                    (uint64_t)rand();
    testSizes[i] = hostmem_uint64_to_string_size(testValues[i]);
  }
}

int main(void) {
  hostmem_mono_timer timeUsed;

  hostmem_mono_timer_init();
  hostmem_mono_timer_reset(&timeUsed);
  prepare_test_data();
  bench_prepared(timeUsed);

  const int stepCount = TEST_VALUES_COUNT * 1000;

  bench_section("unsigned to string");
  bench_step(test_snprintf_uint64, stepCount, "  snprintf", "conversion");
  bench_step(test_uint64_to_string, stepCount, "  hostmem", "conversion");
  bench_step(test_uint64_to_string_known_size, stepCount, "  hostmem, size known", "conversion");
  bench_step(test_uint64_to_string_size, stepCount, "  size only", "conversion");

  bench_section("signed to string");
  bench_step(test_snprintf_int64, stepCount, "  snprintf", "conversion");
  bench_step(test_int64_to_string, stepCount, "  hostmem", "conversion");

  bench_section("duration to string");
  bench_step(test_duration_to_string, stepCount, "  hostmem", "conversion");

  bench_total(timeUsed, stepCount, "value");

  return 0;
}
