#!/bin/bash
# Formatting, plus the three checks that keep this library embeddable.
set -euo pipefail
cd "$(dirname "$0")"

# find, not a ladder of */**/ globs: bash expands ** like a single * unless globstar is set,
# so such a ladder reaches a fixed depth and silently skips whatever sits below it. That is
# fine until someone adds a directory level, and then it is wrong without saying so.
find src include tests/unit/src benchmarks/src \
  \( -name "*.c" -o -name "*.h" -o -name "*.cpp" \) \
  -print0 | xargs -0 clang-format -i

status=0

# Sources stay ASCII. A compiler decides what a byte above 0x7F means by its own rules, and
# MSVC reads a file in the system codepage unless told otherwise -- the same source then means
# something different on another machine, or stops compiling with C4819. clang-format has no
# say in this, so the rule lives here.
#
# The three characters this codebase used to reach for have plain stand-ins that read the same
# in a fixed width font: an em dash is --, an ellipsis is ..., a multiplication sign is x.
#
# perl and not `grep -P`: PCRE is an optional grep feature and absent from the BSD grep macOS
# ships, where the check would have exited 2 and, swallowed by a `|| true`, reported a clean
# file it never read. perl is a hard dependency of this script anyway (see the malloc check
# below) and separates the two outcomes by different signals: output means findings, a non-zero
# exit means the file was not examined. Both fail the lint -- a rule that cannot run is not a
# rule that passed.
#
# The open is spelled out rather than left to `perl -ne`, whose implicit loop only warns about a
# file it cannot read and still exits 0 -- the very hole this check is here to close.
while IFS= read -r -d '' f; do
  rc=0
  hits=$(perl -e 'open(my $fh, "<", $ARGV[0]) or die "$!\n";
                  while (<$fh>) { print "$.: $_" if /[^\x00-\x7F]/ }' "$f") || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "could not scan for non-ASCII bytes (perl exit $rc): $f" >&2
    status=1
  elif [ -n "$hits" ]; then
    echo "non-ASCII bytes (use -- for an em dash, ... for an ellipsis, x for a times sign): $f" >&2
    echo "$hits" | sed 's/^/  /' >&2
    status=1
  fi
done < <(find src include tests/unit/src benchmarks/src \
  \( -name "*.c" -o -name "*.h" -o -name "*.cpp" \) -print0)

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
# memory, and a stray allocation would break that promise silently. Comments are blanked out
# first -- the documentation mentions free() often and rightly so. String literals are matched
# but kept, so a `free(` inside one would be reported; none exists, and a rule that errs toward
# asking is the right way round here.
for f in $(find src include -name "*.c" -o -name "*.h" | grep -v "^src/memory.c$"); do
  # Same care as the ASCII check above: grep answers 1 for "nothing found" and 2 for "I could
  # not look", and a bare `|| true` would report the second as the first. Only 1 is the clean
  # no-match; anything else means this file went unchecked and the lint has to say so.
  rc=0
  stripped=$(perl -e 'open(my $fh, "<", $ARGV[0]) or die "$!\n";
                      local $/; my $s = <$fh>;
                      $s =~ s{("(?:\\.|[^"\\])*")|(/\*.*?\*/)|(//[^\n]*)}
                             {$1 ? $1 : ($2 ? ($2 =~ s/[^\n]/ /gr) : "")}gsex;
                      print $s' "$f") || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "could not strip comments before the allocation check (perl exit $rc): $f" >&2
    status=1
    continue
  fi
  hits=$(printf '%s' "$stripped" | grep -nE "\b(malloc|calloc|realloc|free)\s*\(") || rc=$?
  if [ "$rc" -gt 1 ]; then
    echo "could not scan for allocations (grep exit $rc): $f" >&2
    status=1
  elif [ -n "$hits" ]; then
    echo "allocation outside src/memory.c: $f" >&2
    echo "$hits" | sed 's/^/  /' >&2
    status=1
  fi
done

[ $status -eq 0 ] && echo "lint ok"
exit $status
