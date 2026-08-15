#include "hostmem/utils/converter.h"
#include "hostmem/utils/mono_timer.h"
#include <gtest/gtest.h>

#include "memory_limit.h"
#include <random>
#include <string>

TEST(Converter, hostmem_uint64_to_string) {
  char buffer[20];
  auto expectedSize = hostmem_uint64_to_string(buffer, sizeof(buffer), 123456789);
  EXPECT_EQ(expectedSize, 9);
  EXPECT_STREQ(buffer, "123456789");
}

TEST(Converter, hostmem_uint64_to_string_full) {
  char buffer[20];
  auto expectedSize = hostmem_uint64_to_string(buffer, sizeof(buffer), 1234567890123456789);
  EXPECT_EQ(expectedSize, 19);
  EXPECT_STREQ(buffer, "1234567890123456789");
}

TEST(Converter, hostmem_uint64_to_string_empty) {
  char buffer[20];
  auto expectedSize = hostmem_uint64_to_string(buffer, sizeof(buffer), 0);
  EXPECT_EQ(expectedSize, 1);
  EXPECT_STREQ(buffer, "0");
}

TEST(Converter, hostmem_uint64_to_string_too_small_buffer) {
  char buffer[1];
  auto expectedSize = hostmem_uint64_to_string(buffer, sizeof(buffer), 123456789);
  EXPECT_EQ(expectedSize, 9);
  EXPECT_STREQ(buffer, "");
}

size_t hostmem_uint64_to_string_size_old(uint64_t value) {
  static uint64_t powers[] = {
      10,
      100,
      1000,
      10000,
      100000,
      1000000,
      10000000,
      100000000,
      1000000000,
      10000000000,
      100000000000,
      1000000000000,
      10000000000000,
      100000000000000,
      1000000000000000,
      10000000000000000,
      100000000000000000,
      1000000000000000000,
      10000000000000000000u
  };
  int i = 0;
  while (value >= powers[i] && i < 18) { ++i; }
  return i + 1;
}

TEST(Converter, hostmem_uint64_to_string_size_validation) {
  auto ref = hostmem_uint64_to_string_size_old;
  auto opt = hostmem_uint64_to_string_size;

  // --- 1. Explicit Edge Cases ---
  uint64_t cases[] = {0ULL, 1ULL, 9ULL, 10ULL, 99ULL, 100ULL, 999ULL, 1000ULL, UINT64_MAX};

  for (uint64_t v : cases) { ASSERT_EQ(ref(v), opt(v)) << "Edge case failed: " << v; }

  // --- 2. Boundaries around powers of 10 ---
  uint64_t p = 1;
  for (int d = 1; d <= 19; d++) {
    uint64_t low = p;
    uint64_t high = p * 10 - 1;

    for (int i = -2; i <= 2; i++) {
      if (low + i > 0) { ASSERT_EQ(ref(low + i), opt(low + i)) << "Low boundary: " << (low + i); }

      ASSERT_EQ(ref(high + i), opt(high + i)) << "High boundary: " << (high + i);
    }

    p *= 10;
  }

  // --- 3. Bit-structure tests (logarithmic distribution) ---
  for (uint64_t v = 1; v != 0; v <<= 1) {
    ASSERT_EQ(ref(v), opt(v)) << "Power of 2: " << v;

    if (v > 0) ASSERT_EQ(ref(v - 1), opt(v - 1));
    if (v < UINT64_MAX) ASSERT_EQ(ref(v + 1), opt(v + 1));
  }

  // --- 4. Random values ---
  std::mt19937_64 rng(123456);

  for (size_t i = 0; i < 5'000'000; i++) {
    uint64_t v = rng();
    ASSERT_EQ(ref(v), opt(v));
  }
}

// INT64_MIN is the one value that cannot be negated in int64_t: `v * -1` is undefined there,
// and the result only looked right because two's complement wrapping happened to land on it.
TEST(ConverterInt64, HandlesInt64Min) {
  const std::string expected = std::to_string(INT64_MIN); // "-9223372036854775808"
  ASSERT_EQ(expected.size(), 20u);

  EXPECT_EQ(hostmem_int64_to_string_size(INT64_MIN), expected.size());

  char buffer[32] = {};
  const size_t written =
      hostmem_int64_to_string_known_string_size(buffer, INT64_MIN, expected.size());
  EXPECT_EQ(written, expected.size());
  EXPECT_STREQ(buffer, expected.c_str());

  // and the neighbours, so an off by one in the negation would not slip through
  EXPECT_EQ(hostmem_int64_to_string_size(INT64_MIN + 1), 20u);
  EXPECT_EQ(hostmem_int64_to_string_size(INT64_MAX), 19u);
  EXPECT_EQ(hostmem_int64_to_string_size(-1), 2u);
  EXPECT_EQ(hostmem_int64_to_string_size(0), 1u);
}
