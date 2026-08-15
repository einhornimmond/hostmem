# AGENTS.md – hostmem

A **C11** library meant to be linked into a program written in something else. One sentence
carries the whole design: **the host owns the memory.** The caller hands over a blob, hostmem
works inside it, the blob goes back unchanged in size and ownership.

Everything below follows from that. The **memory contract** and the **commenting standard**
are not negotiable; the rest is what saves you a wasted afternoon.

----------

## Build and test

zig only — there is no CMake here. **`-Dtarget` is mandatory**; a native build without it
fails.

```bash
zig build -Dtarget=x86_64-linux-gnu -Dtests=true -Dbenchmarks=true
./run_all.sh                 # every binary in zig-out/bin, one line each
./run_all.sh --tests         # skip the benchmarks
./lint.sh                    # format + the two structural checks below
```

`lint.sh` walks `src/`, `include/`, `tests/unit/src/` and `benchmarks/src/` with `find`, so it
covers `.c`, `.h` and `.cpp` at any depth. Do not replace that with a ladder of `src/**/*.c`
patterns: bash expands `**` like a single `*` unless `globstar` is set, so such a ladder stops
at a fixed depth and skips anything below it without a word.

| Option | Meaning |
|---|---|
| `-Dtarget=` | required, e.g. `x86_64-linux-gnu`, `x86_64-linux-musl`, `x86_64-windows-gnu`, `aarch64-macos` |
| `-Dtests=true` | build the googletest binaries |
| `-Dbenchmarks=true` | build the `bench_*` binaries |
| `-Dshared=true` | dynamic library — what a language binding usually wants |
| `-Dsanitize=undefined_behavior` | UBSan; `thread` for TSan. AddressSanitizer is not available through zig |

**Benchmarks need `-Doptimize=ReleaseFast`.** In a debug build the hand written digit loop
loses to `snprintf` by a factor of two, because libc ships optimised and your build does not.
A benchmark number from a debug build is not wrong, it is answering a different question —
label it or do not report it.

**Tests cap their own memory.** `tests/unit/src/memory_limit.h` sets `RLIMIT_AS` to 2048 MB on
Linux, skipped under sanitizers. Raise it with `HOSTMEM_TEST_MEMORY_LIMIT_MB=8192`, disable
with `0`. Include it in every new test binary. It exists because a boundary test once
allocated 64 GB before anyone could reach Ctrl-C.

----------

## The memory contract

- **`malloc` appears in exactly one file: `src/memory.c`, on the NULL-allocator path.**
  Nowhere else, ever. `lint.sh` fails if a second one appears, and that check is the point of
  the library, not a formality.
- **Sizes are `uint32_t`** — counts, indices, byte sizes alike. Anything that would not fit
  returns `HOSTMEM_ERROR_ARITHMETIC_OVERFLOW` rather than wrapping. Where a bound is known at
  compile time, use `static_assert` instead of a runtime check.
- **Sizes are passed in, never stored.** Freeing and resizing need the size the caller
  allocated with; a wrong size moves the arena index by the wrong amount and hands the same
  bytes out twice. `hostmem_memory_block` keeps pointer and size together when that
  bookkeeping should not be the caller's job.
- **Every size rounds up to a multiple of 8**, which keeps every returned pointer 8 byte
  aligned.
- **`HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED` is neither success nor failure**: the
  operation happened, the memory did not come back. Handle it explicitly at each call site and
  compare against the exact value. No `if (ok(result))` helper — a reader has to see that this
  warning can arrive here, and be able to decide anew what it should mean.
- **Failures leave every output untouched.** Init functions write every field and read none,
  so uninitialised storage is a valid input.
- **No hidden state.** No globals holding allocators, no thread-local caches, no atexit
  handlers. A host may load this library twice into one process.

`HOSTMEM_ERROR_USER_BASE` reserves the code range above 1000 for the embedding project. Codes
below it belong to hostmem and may gain members between releases.

----------

## Portability contract

Targets: Linux (glibc, musl), Windows (MinGW; the MSVC ABI needs the MSVC SDK present), macOS
on both architectures. Never claim a target you did not build.

- **No C++ headers in C code.** `<cstdint>` in a `.c` file breaks every C compiler; use
  `<stdint.h>`.
- **Every public header compiles on its own, as C and as C++.** `lint.sh` checks all of them —
  a consumer may include exactly one header and nothing else.
- **The `static_assert` fallback must exclude C++**, where it is a keyword and not a macro:
  ```c
  #if !defined(__cplusplus) && !defined(static_assert)
  #define static_assert _Static_assert
  #endif
  ```
- **No legacy or POSIX-only headers** without a guard. `<memory.h>` is a removed SVID relic;
  `<unistd.h>` and `<sys/*.h>` need an `#ifdef`.
- **Platform branches carry their own includes.** A `#ifdef _WIN32` block calling `exit()`
  needs `<stdlib.h>`; that `windows.h` drags it in is luck, not a contract.
- **Every `.c` includes its own header first**, so the compiler checks declaration against
  definition.
- **Nothing vendored.** There is no `third_party/`. A dependency here becomes a dependency for
  every host that embeds this, in every language — the bar is accordingly high.

----------

## Naming

Every public symbol starts with `hostmem_`, every macro with `HOSTMEM_`. The allocator type is
`hostmem` itself, so its operations read as `hostmem_init_arena`, `hostmem_alloc`,
`hostmem_reset`. `hostmem_release` returns the arena buffer; `hostmem_destroy` releases a
descriptor that came from `hostmem_create`.

----------

## How to work in this repository

- **Measure before you claim.** Object sizes, timings, "this is faster" — run it, in a release
  build. A compiler often optimises away exactly the thing you were about to take credit for.
- **Prove the test bites.** After fixing something, revert the fix, watch the new test fail,
  then put it back. A test that never failed proves nothing. The same goes for a new lint
  rule.
- **Cap the memory before probing a boundary.** `ulimit -v` for a scratch program,
  `memory_limit.h` for a test binary. Prefer a small static arena over malloc when the point is
  that a request must be *refused*.
- **Say what you did not verify.** A target you could not build, a platform you do not have —
  name it. Silence reads as confirmation.
- **Keep the diff about the change.** Reformatting, drive-by renames and speculative refactors
  make review expensive. Format the files you touched; put a reformatting pass in its own
  commit.

----------

## C Modules (Doxygen)

- Every public C header MUST define exactly one module using `@defgroup`.
- The module MUST wrap the API using `@{` … `@}`.

```c
/** @defgroup hostmem_memory hostmem_memory
  *  @ingroup utils
  *  @brief Allocator that is either a bump arena or plain malloc/free
  *  @{
  */

// API here

/** @} */
```

- Modules MUST belong to a parent via `@ingroup` with their folder name (`utils`).
- If the parent does not exist, DEFINE it once:

```c
/** @defgroup utils Utilities */
```

### Rules

- One module per header
- All public API must be inside the module block
- Use flat, stable identifiers (`hostmem_memory`)

----------

## Commenting Guidelines, Poetic Precision – Dual-Layer Commenting Standard

All comments consist of two aligned layers.

### 1. Technical Layer (Ground Truth)

Hard, verifiable specification. Must include: parameters, types, constraints, edge cases,
return behaviour, overflow and limits, deterministic rules. No ambiguity, no metaphor instead
of facts, and sufficient on its own for an implementation without the poetic layer.

### 2. Semantic Layer (Poetic Precision)

Describes system behaviour as natural process perception: flow, cycle, rhythm, transition;
dissolve, emerge, settle, converge; stream, season, tide, growth, decay.

Constraints: must not change technical meaning, must not introduce moral framing, must not
replace constraints with imagery, must stay fact-consistent.

Purpose: reduce cognitive load, improve conceptual continuity, express system behaviour as a
continuous process.

### Forbidden Transformations

Do NOT convert constraints into metaphors only, limits into value judgements, edge cases into
poetic ambiguity, or precision into narrative softness.

### Writing Principle

Each comment is **deterministic logic + natural process description**. Never poetry instead of
specification, never specification without semantic flow.

### The `@whisper` Tag – Optional Poetic Signature

An optional, poetic one-liner at the end of a Doxygen comment. Encouraged for functions that
carry significant meaning, rare for low-level helpers. It must describe the essence of the
function in calm language, stay subtle, and end without a period. It must never replace
missing technical documentation, never preach, and never drift into irony.

**Never delete an existing `@whisper`** unless it has become unrelated to the function's
behaviour. Updating is allowed when the function changed enough that the old line no longer
fits — rewrite it in the same tone. Do not change one for stylistic preference.

### Standard Comment Structure (Flexible)

```c
/**
 * @brief One-line summary (poetic but clear).
 *
 * A few sentences explaining what the function does. Use calm, image-rich language.
 * Mention technical details naturally within the flow.
 *
 * @param[in/out] name   Description.
 * @return               Exact return values.
 * @note (optional)      Important constraints.
 * @whisper (optional)   Short poetic line, no period.
 */
```

### What to Avoid

Preaching ("should", "must", "good", "fair"). Exclamation marks. Floating-point illusions.
Deleting or editing an existing `@whisper` unless the function changed completely.

----------

**Remember:** the goal is not perfect technical prose. It is to make reading the code a quiet
pleasure — accurate, calm, and a little beautiful.
