#include "hostmem/duration.h"
#include "memory_limit.h"
#include <gtest/gtest.h>

TEST(Duration, Ns) {
  char buffer[10];
  hostmem_duration_string(buffer, sizeof(buffer), (hostmem_duration)500, 0);
  EXPECT_STREQ(buffer, "500 ns");
}

TEST(Duration, Microseconds) {
  char buffer[10];
  hostmem_duration_string(buffer, sizeof(buffer), (hostmem_duration)1234, 3);
  EXPECT_STREQ(buffer, "1.234 us");
}

TEST(Duration, Milliseconds) {
  char buffer[10];
  hostmem_duration_string(buffer, sizeof(buffer), (hostmem_duration)1500000LL, 3);
  EXPECT_STREQ(buffer, "1.500 ms");
}

TEST(Duration, Seconds) {
  char buffer[10];
  hostmem_duration_string(buffer, sizeof(buffer), (hostmem_duration)1500000000LL, 3);
  EXPECT_STREQ(buffer, "1.500 s");
}

TEST(Duration, Minutes) {
  char buffer[10];
  hostmem_duration ns = 90LL * 1000000000LL; // 90 seconds -> 1.5 minutes
  hostmem_duration_string(buffer, sizeof(buffer), ns, 3);
  EXPECT_STREQ(buffer, "1.500 min");
}

TEST(Duration, Hours) {
  char buffer[10];
  hostmem_duration ns = 3ULL * 3600ULL * 1000000000ULL; // 3 hours
  hostmem_duration_string(buffer, sizeof(buffer), ns, 3);
  EXPECT_STREQ(buffer, "3.000 h");
}

TEST(Duration, Days) {
  char buffer[10];
  hostmem_duration ns = 36ULL * 3600ULL * 1000000000ULL; // 36 hours -> 1.5 days
  hostmem_duration_string(buffer, sizeof(buffer), ns, 1);
  EXPECT_STREQ(buffer, "1.5 days");
}

TEST(Duration, DaysMoreDecimal) {
  char buffer[25];
  hostmem_duration ns = 36ULL * 3600ULL * 1500700030ULL;
  hostmem_duration_string(buffer, sizeof(buffer), ns, 10);
  EXPECT_STREQ(buffer, "2.2510500449 days");
}

TEST(Duration, CombinedDurations) {
  hostmem_duration duration = (hostmem_duration)1 * 24ULL * 3600ULL * 1000000000ULL + // 1 day
                              (hostmem_duration)2 * 3600ULL * 1000000000ULL +         // 2 hours
                              (hostmem_duration)3 * 60ULL * 1000000000ULL +           // 3 minutes
                              (hostmem_duration)4 * 1000000000ULL;                    // 4 seconds
  EXPECT_EQ(duration, 93784000000000ULL);
  char buffer[20];
  hostmem_duration_string(buffer, sizeof(buffer), duration, 2);
  EXPECT_STREQ(buffer, "1.08 days");
  hostmem_duration_string(buffer, sizeof(buffer), duration, 3);
  EXPECT_STREQ(buffer, "1.085 days");
  hostmem_duration_string(buffer, sizeof(buffer), duration, 4);
  EXPECT_STREQ(buffer, "1.0854 days");
  hostmem_duration_string(buffer, sizeof(buffer), duration, 8);
  EXPECT_STREQ(buffer, "1.08546296 days");
}
