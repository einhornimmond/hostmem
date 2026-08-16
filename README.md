# hostmem

C primitives for embedding. **The host owns the memory.**

A small C11 library meant to be linked into a program written in something else — Node.js, Go,
Rust, C++. It brings a bump allocator, containers built on it, and the conversions a host does
constantly and the standard library does slowly. What it does not bring is a memory policy of
its own: you hand it a blob, it works inside that blob, and it gives the blob back.

```c
#include "hostmem/memory.h"
#include "hostmem/bucket_vector.h"

uint8_t blob[64 * 1024];          // wherever this comes from: the host decides
hostmem mem;
hostmem_init_arena_borrow(&mem, blob, sizeof(blob));

uint8_t *buffer = NULL;
hostmem_alloc(&buffer, 128, &mem);
/* ... */
hostmem_free(buffer, 128, &mem);

hostmem_reset(&mem);              // the whole arena, in one move
```

Pass `NULL` instead of an allocator and every call falls back to malloc/free. That is the only
place in the library where `malloc` appears — `lint.sh` fails the build if a second one shows
up.

## What is in it

| Header | What it gives you |
|---|---|
| `hostmem/memory.h` | bump arena or malloc/free, chosen per call by what you pass |
| `hostmem/memory_block.h` | pointer and size kept together, so freeing needs no bookkeeping from you |
| `hostmem/multi_arena.h` | a chain of arenas that opens another one instead of running dry |
| `hostmem/bucket_vector.h` | growing sequence with stable element addresses; no copy on growth |
| `hostmem/converter.h` | integer to decimal string, roughly 4× faster than `snprintf` |
| `hostmem/duration.h` | nanoseconds to a readable span |
| `hostmem/mono_timer.h` | monotonic clock, one type, three units |
| `hostmem/result.h` | one result code for everything, with a range reserved for you |

## The contract

- **Sizes are `uint32_t`.** Counts, indices, byte sizes. Anything that would not fit returns
  `HOSTMEM_ERROR_ARITHMETIC_OVERFLOW` instead of wrapping.
- **Sizes are passed in, never stored.** Freeing and resizing need the size you allocated
  with. `hostmem_memory_block` keeps the two together when you would rather not.
- **Every size rounds up to 8**, so every pointer the arena hands out is 8 byte aligned.
- **An arena only takes back its most recent allocation.** Anything before it stays until
  `hostmem_reset`. Calls that could not reclaim return
  `HOSTMEM_WARNING_ARENA_MEMORY_NOT_RECLAIMED` — the operation happened, the memory did not
  come back. It is neither a failure nor a release; handle it where it appears.
- **Failures leave every output untouched.**

## When one arena is not enough

An arena has the capacity it was born with. `hostmem_multi_arena` keeps a chain of them and
opens the next one when the current stretch fills, so the size never has to be guessed right up
front — and a request larger than the arena capacity gets ground of its own instead of a
refusal.

```c
#include "hostmem/multi_arena.h"

hostmem_multi_arena chain;
// 1 MiB per arena, and an arena drops out of the search once under 4 KiB is left.
// 0 for either takes the default: 1 MiB and 128 bytes.
hostmem_multi_arena_init(&chain, 1024 * 1024, 4096, NULL);
hostmem_multi_arena_borrow(&chain, blob, sizeof(blob)); // optional: lend it the host's blob

uint8_t *buffer = NULL;
hostmem_multi_arena_alloc(&buffer, 4096, &chain);

hostmem_multi_arena_reset(&chain);    // every allocation, in one move; the arenas stay
hostmem_multi_arena_shrink(&chain);   // and give the empty ones back to the host
hostmem_multi_arena_release(&chain);
```

Pointers stay put: an arena, once opened, is never moved or resized. A borrowed block stays
the host's: the chain fills it and never frees it. `NULL` is not a malloc fallback here — a chain has to exist.

The third argument is the one worth thinking about. A request is served first fit, and an arena
leaves that search for good once its remainder falls to the threshold. So the question it
answers is: what is the smallest request that should still land in a leftover? An arena holds a
request of `n` bytes while at least `n` are left, and is written off at or below the threshold,
so `n - 8` drops it out of the search exactly when it can no longer take that request.

For uniform records that is the whole story — one alignment step below the record size wastes
nothing and keeps the search short. For mixed sizes it is a trade: lower gives up nothing usable
but leaves arenas in the search holding remainders only the small requests fit, at around half a
nanosecond per arena walked, which only bites once a thousand of them have piled up; higher
keeps the search short and writes off up to a threshold worth of bytes per arena.
`bench_multi_arena` puts numbers on both ends.

## Build

```bash
zig build -Dtarget=x86_64-linux-gnu                          # the static library
zig build -Dtarget=x86_64-linux-gnu -Dtests=true -Dbenchmarks=true
./run_all.sh                                                 # run everything in zig-out/bin
```

`-Dtarget` is required. Further options: `-Dshared=true` for a dynamic library (what a
language binding usually wants), `-Dsanitize=undefined_behavior`, `-DsingleOutputDir=true` to
drop the artifacts without the `bin/` and `lib/` split.

Targets verified to build: `x86_64-linux-gnu`, `x86_64-linux-musl`, `x86_64-windows-gnu`,
`x86_64-macos`, `aarch64-macos`. The `-msvc` ABI targets need the MSVC SDK headers present on
the machine — zig does not ship them, so that combination is untested here.

**Benchmark numbers need `-Doptimize=ReleaseFast`.** In a debug build the hand written digit
loop loses to `snprintf` by a factor of two, because libc ships optimised and your build does
not. With optimisation the picture turns around:

```
unsigned to string        snprintf   61.8 ns      hostmem  16.3 ns
signed to string          snprintf   58.2 ns      hostmem  13.8 ns
```

## Using it from another zig project

```zig
const hostmem = b.dependency("hostmem", .{ .target = target, .optimize = optimize });
lib.linkLibrary(hostmem.artifact("hostmem"));
lib.addIncludePath(hostmem.path("include"));
```

## License

Apache 2.0
