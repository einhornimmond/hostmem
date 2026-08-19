#include "bench_report.h"
#include "hostmem/converter.h"
#include "hostmem/memory_block.h"
#include "hostmem/mono_timer.h"
#include "hostmem/result.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * What this benchmark measures
 *
 * Both directions of the byte-to-text conversions -- hex over a whole block, and a uuid in its
 * canonical 8-4-4-4-12 form -- on their own, no baseline beside them. hostmem links no crypto
 * library, so the constant time conversions such a library ships are not here to compare
 * against, and a printf loop would answer a question nobody asks in a hot path.
 *
 * What the columns do say is how the cost grows. Each section fixes an input length and reports
 * the nanoseconds one whole conversion takes, so the per byte cost falls out of dividing by the
 * length: a short input pays mostly for the call and the length check, while a long one settles
 * into the loop the compiler vectorised, and the two numbers per byte are far apart.
 *
 * The lengths are the ones that actually turn up: 16 bytes for a uuid, 32 for a hash or a public
 * key, 64 for a signature, 1024 for a serialised record. The uuid section converts the same 16
 * bytes through the table driven path, so the two rows at 16 bytes say what the separators and
 * the scattered positions cost.
 *
 * Numbers from a debug build answer a different question -- there is no vector body there at
 * all. Build with -Doptimize=ReleaseFast before reporting any of them.
 */

#define MAX_PAYLOAD_SIZE 1024
#define PAYLOAD_VARIANTS 64

/*
 * Several payloads rather than one, cycled through: a single buffer converted a million times
 * sits in L1 and reports a cache the real caller will not have. The hex strings are prepared
 * once from the same payloads, so the decoding steps read valid input and never take the
 * failure path.
 */
static uint8_t payloads[PAYLOAD_VARIANTS][MAX_PAYLOAD_SIZE];
static char hexStrings[PAYLOAD_VARIANTS][MAX_PAYLOAD_SIZE * 2 + 1];

/** The same payloads rendered as uuids, prepared once so the decoding step reads valid input. */
static char uuidStrings[PAYLOAD_VARIANTS][HOSTMEM_UUID_STRING_LENGTH + 1];

/** Receives every conversion, so the compiler cannot drop the call. */
static char benchHexBuffer[MAX_PAYLOAD_SIZE * 2 + 1];
static uint8_t benchBinaryBuffer[MAX_PAYLOAD_SIZE];
static char benchUuidBuffer[HOSTMEM_UUID_STRING_LENGTH + 1];

static int cursor = 0;
/** Set by the driver before each step; the step functions take their length from here. */
static uint32_t currentLength = 0;
/** Folds every result code in, so a silent failure cannot hide behind a fast number. */
static unsigned resultSink = 0;
/** The uuid encoding returns nothing, so a byte of what it wrote stands in for a result code. */
static unsigned writtenSink = 0;

static int nextVariant(void) {
  int result = cursor++;
  if (cursor >= PAYLOAD_VARIANTS) { cursor = 0; }
  return result;
}

static void test_binary_to_hex(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    hostmem_memory_block block = {payloads[nextVariant()], currentLength};
    resultSink |= (unsigned)hostmem_binary_to_hex(benchHexBuffer, &block);
  }
}

static void test_binary_from_hex(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    // the string is terminated at twice the current length, so only that much is read
    resultSink |= (unsigned)hostmem_binary_from_hex(benchBinaryBuffer, hexStrings[nextVariant()]);
  }
}

static void test_uuid_to_string(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    hostmem_uuid_to_string(benchUuidBuffer, payloads[nextVariant()]);
  }
  writtenSink |= (unsigned char)benchUuidBuffer[0];
}

static void test_uuid_from_string(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    resultSink |= (unsigned)hostmem_uuid_from_string(benchBinaryBuffer, uuidStrings[nextVariant()]);
  }
}

/* --- driver ------------------------------------------------------------------------------ */

static void prepare_test_data(void) {
  srand(4711);
  for (int v = 0; v < PAYLOAD_VARIANTS; ++v) {
    for (int i = 0; i < MAX_PAYLOAD_SIZE; ++i) { payloads[v][i] = (uint8_t)(rand() & 0xFF); }
    hostmem_uuid_to_string(uuidStrings[v], payloads[v]);
  }
}

/*
 * Terminates every prepared string at twice the length about to be measured, so the decoding
 * step reads exactly as many characters as the encoding step wrote. Called once per section.
 */
static void set_length(uint32_t length) {
  currentLength = length;
  for (int v = 0; v < PAYLOAD_VARIANTS; ++v) {
    hostmem_memory_block block = {payloads[v], length};
    if (HOSTMEM_SUCCESS != hostmem_binary_to_hex(hexStrings[v], &block)) {
      printf("could not prepare the hex strings\n");
      exit(1);
    }
  }
  cursor = 0;
}

int main(void) {
  hostmem_mono_timer timeUsed;
  static const uint32_t lengths[] = {16, 32, 64, 1024};

  hostmem_mono_timer_init();
  hostmem_mono_timer_reset(&timeUsed);
  prepare_test_data();
  bench_prepared(timeUsed);

  for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); ++l) {
    char heading[64];
    // a long input costs orders of magnitude more per conversion than a short one, so each
    // section buys its own step count rather than dragging the whole run to the slowest
    const int stepCount = lengths[l] >= 1024 ? 200000 : 4000000;

    set_length(lengths[l]);
    snprintf(heading, sizeof(heading), "%u bytes, %d conversions", lengths[l], stepCount);
    bench_section(heading);
    bench_step(test_binary_to_hex, stepCount, "  binary to hex", "conversion");
    bench_step(test_binary_from_hex, stepCount, "  binary from hex", "conversion");
  }

  {
    const int stepCount = 4000000;
    char heading[64];
    cursor = 0;
    snprintf(heading, sizeof(heading), "uuid, %d conversions", stepCount);
    bench_section(heading);
    bench_step(test_uuid_to_string, stepCount, "  uuid to string", "conversion");
    bench_step(test_uuid_from_string, stepCount, "  uuid from string", "conversion");
  }

  bench_total_time(timeUsed);
  // read once at the end: without this the compiler may drop every call whose result nobody
  // wanted, and the benchmark would time an empty loop
  if (resultSink != (unsigned)HOSTMEM_SUCCESS) { printf("a conversion failed: %u\n", resultSink); }
  if (!writtenSink) { printf("the uuid encoding wrote nothing\n"); }
  return 0;
}
