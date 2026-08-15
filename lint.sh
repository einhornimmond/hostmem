#!/bin/bash
# Formatting, plus the two checks that keep this library embeddable.
set -uo pipefail
cd "$(dirname "$0")"

clang-format -i src/*.c
clang-format -i src/**/*.c
clang-format -i include/**/*.h
clang-format -i include/**/**/*.h
clang-format -i tests/unit/src/*.cpp
clang-format -i tests/unit/src/*.h
clang-format -i benchmarks/src/*.c
clang-format -i benchmarks/src/*.h

status=0

# Every public header has to compile on its own, as C and as C++. A type borrowed from
# another header's includes works until that header is tidied; this catches it on the spot.
INC="-Iinclude"
for h in $(find include -name "*.h" | sort); do
  if ! gcc -std=c11 $INC -fsyntax-only -x c "$h" 2>/dev/null; then
    echo "not self contained (C):   $h" >&2
    status=1
  fi
  if ! g++ -std=c++17 $INC -fsyntax-only -x c++ "$h" 2>/dev/null; then
    echo "not self contained (C++): $h" >&2
    status=1
  fi
done

# malloc lives in exactly one place. Everywhere else the caller's blob is the only source of
# memory, and a stray allocation would break that promise silently. Comments and string
# literals are blanked out first — the documentation mentions free() often and rightly so.
for f in $(find src include -name "*.c" -o -name "*.h" | grep -v "^src/memory.c$"); do
  hits=$(perl -0777 -pe 's{("(?:\\.|[^"\\])*")|(/\*.*?\*/)|(//[^\n]*)}
                         {$1 ? $1 : ($2 ? ($2 =~ s/[^\n]/ /gr) : "")}gsex' "$f" \
         | grep -nE "\b(malloc|calloc|realloc|free)\s*\(" || true)
  if [ -n "$hits" ]; then
    echo "allocation outside src/memory.c: $f" >&2
    echo "$hits" | sed 's/^/  /' >&2
    status=1
  fi
done

[ $status -eq 0 ] && echo "lint ok"
exit $status
