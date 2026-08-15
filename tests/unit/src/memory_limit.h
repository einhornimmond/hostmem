#ifndef HOSTMEM_TESTS_MEMORY_LIMIT_H
#define HOSTMEM_TESTS_MEMORY_LIMIT_H

/*
 * Caps the address space of a test binary, so that a test asking for an absurd allocation
 * fails inside the process instead of pulling the whole machine into swap. A bucket vector
 * reserve with a bad bound once claimed 64 GB before anyone could stop it; that is a lost
 * afternoon, while an allocation failure is a red test.
 *
 * The cap sits in the binary rather than in a ctest wrapper on purpose: test binaries get run
 * straight from zig-out/bin at least as often as through ctest, and that is exactly when
 * nothing else is watching.
 *
 * Every unit test .cpp includes this. The whole suite peaks well under 1 GB, so the default
 * leaves generous headroom and still stops a runaway an order of magnitude short of hurting.
 *
 * Override with HOSTMEM_TEST_MEMORY_LIMIT_MB, e.g. for a deliberately large test run:
 *   HOSTMEM_TEST_MEMORY_LIMIT_MB=8192 ./zig-out/bin/test_bucket_vector
 *   HOSTMEM_TEST_MEMORY_LIMIT_MB=0    ./zig-out/bin/test_bucket_vector   # off
 *
 * Linux only. Sanitizer builds are skipped: ASan and TSan reserve terabytes of address space
 * up front, so any RLIMIT_AS would stop them from starting at all.
 */

#if defined(__linux__)

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define HOSTMEM_TEST_SKIP_MEMORY_LIMIT 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define HOSTMEM_TEST_SKIP_MEMORY_LIMIT 1
#endif
#endif

#if !defined(HOSTMEM_TEST_SKIP_MEMORY_LIMIT)

#include <cstdlib>
#include <sys/resource.h>

namespace {

constexpr rlim_t kHostmemTestMemoryLimitDefaultMb = 2048;

struct HostmemTestMemoryLimit {
  HostmemTestMemoryLimit() {
    rlim_t megabytes = kHostmemTestMemoryLimitDefaultMb;
    if (const char *env = std::getenv("HOSTMEM_TEST_MEMORY_LIMIT_MB")) {
      char *end = nullptr;
      const unsigned long long parsed = std::strtoull(env, &end, 10);
      if (end == env || *end != '\0') { return; } // unparsable: leave the process alone
      if (parsed == 0) { return; }                // explicitly disabled
      megabytes = static_cast<rlim_t>(parsed);
    }

    rlimit limit{};
    if (getrlimit(RLIMIT_AS, &limit) != 0) { return; }

    const rlim_t wanted = megabytes * 1024 * 1024;
    // never loosen what the environment already decided, only tighten
    if (limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur <= wanted) { return; }
    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < wanted) { return; }

    limit.rlim_cur = wanted;
    setrlimit(RLIMIT_AS, &limit); // best effort; a refusal just leaves the cap off
  }
};

const HostmemTestMemoryLimit g_hostmem_test_memory_limit;

} // namespace

#endif // !HOSTMEM_TEST_SKIP_MEMORY_LIMIT
#endif // __linux__

#endif // HOSTMEM_TESTS_MEMORY_LIMIT_H
