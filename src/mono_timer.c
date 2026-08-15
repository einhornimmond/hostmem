#include "hostmem/mono_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "hostmem/duration.h"

#ifdef _WIN32
#include <windows.h>

// counts per second
static LARGE_INTEGER freq = {.QuadPart = 0};

// for support more platforms, look into this as example:
// https://github.com/siu/minunit/blob/master/minunit.h
static int64_t get_time_ns() {
  if (freq.QuadPart == 0) { hostmem_mono_timer_init(); }

  LARGE_INTEGER counter;
  if (!QueryPerformanceCounter(&counter)) {
    fprintf(stderr, "Error: QueryPerformanceCounter failed\n");
    exit(1);
  }
  // The counter is scaled to nanoseconds in 128 bit, so a machine that has been up for a
  // while cannot overflow the multiplication. The build is zig only and therefore always
  // clang, which carries __int128 on every target — no fixed point fallback is needed.
  __int128 tmp = (__int128)counter.QuadPart * 1000000000LL;
  return (int64_t)(tmp / (int64_t)freq.QuadPart);
}

#else

static int64_t get_time_ns() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}

#endif

bool hostmem_mono_timer_init() {
#ifdef _WIN32
  if (!QueryPerformanceFrequency(&freq)) {
    fprintf(stderr, "Error: QueryPerformanceFrequency failed\n");
    return false;
  }
#endif
  return true;
}

void hostmem_mono_timer_reset(hostmem_mono_timer *start) {
  *start = get_time_ns();
}

int64_t hostmem_mono_timer_nanos(hostmem_mono_timer start) {
  return get_time_ns() - start;
}

double hostmem_mono_timer_micros(hostmem_mono_timer start) {
  return (double)hostmem_mono_timer_nanos(start) / 1e3;
}

double hostmem_mono_timer_millis(hostmem_mono_timer start) {
  return (double)hostmem_mono_timer_nanos(start) / 1e6;
}

double hostmem_mono_timer_seconds(hostmem_mono_timer start) {
  return (double)hostmem_mono_timer_nanos(start) / 1e9;
}
int hostmem_mono_timer_string(char *buffer, size_t buffer_size, hostmem_mono_timer start) {
  return hostmem_duration_string(buffer, buffer_size, hostmem_mono_timer_nanos(start), 4);
}
