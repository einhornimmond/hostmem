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
hostmem_init_arena_static(&mem, blob, sizeof(blob));

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
