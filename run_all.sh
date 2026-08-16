#!/usr/bin/env bash
#
# Runs every executable in a build output directory and reports one line each.
#
#   ./run_all.sh                     # everything in zig-out/bin
#   ./run_all.sh -o memory           # only binaries whose name contains "memory"
#   ./run_all.sh --tests             # skip the bench_* binaries
#   ./run_all.sh -- --gtest_filter='*Arena*'    # pass arguments through to each binary
#
# A gtest binary reports a verdict, so only its failures are worth printing. Anything else --
# the bench_* binaries -- has its output *as* the result, so that is always shown. -v prints
# everything, -q only failures. Exits non-zero when any binary fails.

set -uo pipefail

DIR=""
FILTER=""
KIND="all"
VERBOSE=0
QUIET=0
TIMEOUT=600
PASS_ARGS=()

usage() {
  cat <<'EOF'
Runs every executable in a build output directory and reports one line each.

  ./run_all.sh                              everything in zig-out/bin
  ./run_all.sh -o memory                    only binaries whose name contains "memory"
  ./run_all.sh --tests                      skip the bench_* binaries
  ./run_all.sh -- --gtest_filter='*Arena*'  pass arguments through to each binary

Test binaries report a verdict, so only their failures are printed. Binaries without one --
the benchmarks -- have their output as the result, so it is always shown.
Exits non-zero when any binary fails.

Options:
  -d, --dir DIR        directory to scan (default: zig-out/bin)
  -o, --only PATTERN   run only binaries whose name contains PATTERN
      --tests          skip bench_* binaries
      --bench          run only bench_* binaries
  -t, --timeout SEC    per binary timeout, 0 disables (default: 600)
  -v, --verbose        show every binary's output
  -q, --quiet          show output only for failures, benchmarks included
  -h, --help           this text
  --                   everything after this is passed to every binary
EOF
}

# Without this an option missing its value would leave $# at 1, where `shift 2` fails
# without shifting -- and the loop would spin on the same argument forever.
require_value() {
  if [ "$2" -lt 2 ]; then
    echo "missing value for $1" >&2
    usage >&2
    exit 2
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    -d|--dir)     require_value "$1" $#; DIR="$2"; shift 2 ;;
    -o|--only)    require_value "$1" $#; FILTER="$2"; shift 2 ;;
    --tests)      KIND="tests"; shift ;;
    --bench)      KIND="bench"; shift ;;
    -t|--timeout) require_value "$1" $#; TIMEOUT="$2"; shift 2 ;;
    -v|--verbose) VERBOSE=1; shift ;;
    -q|--quiet)   QUIET=1; shift ;;
    -h|--help)    usage; exit 0 ;;
    --)           shift; PASS_ARGS=("$@"); break ;;
    *)            echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")" || exit 1

if [ -z "$DIR" ]; then
  for candidate in zig-out/bin; do
    if [ -d "$candidate" ]; then DIR="$candidate"; break; fi
  done
fi
if [ -z "$DIR" ] || [ ! -d "$DIR" ]; then
  echo "no build output found -- looked for zig-out/bin" >&2
  echo "build first, e.g.: zig build -Dtests=true -Dbenchmarks=true" >&2
  exit 1
fi

# colours only when someone is watching
if [ -t 1 ]; then
  GREEN=$'\033[0;32m'; RED=$'\033[0;31m'; DIM=$'\033[2m'; RESET=$'\033[0m'
else
  GREEN=""; RED=""; DIM=""; RESET=""
fi

# bash 5 has microseconds; older shells fall back to whole seconds
now_ms() {
  if [ -n "${EPOCHREALTIME-}" ]; then
    local t="${EPOCHREALTIME/,/.}"   # some locales use a comma
    echo $(( ${t%.*} * 1000 + 10#${t#*.} / 1000 ))
  else
    echo $(( SECONDS * 1000 ))
  fi
}

binaries=()
while IFS= read -r path; do
  name="$(basename "$path")"
  case "$name" in
    *.o|*.a|*.so|*.dll|*.pdb|*.lib) continue ;;
  esac
  [ -n "$FILTER" ] && case "$name" in *"$FILTER"*) ;; *) continue ;; esac
  case "$KIND" in
    tests) case "$name" in bench_*) continue ;; esac ;;
    bench) case "$name" in bench_*) ;; *) continue ;; esac ;;
  esac
  binaries+=("$path")
done < <(find "$DIR" -maxdepth 1 -type f -perm -u+x | sort)

if [ ${#binaries[@]} -eq 0 ]; then
  echo "no matching executables in $DIR" >&2
  exit 1
fi

runner=()
if [ "$TIMEOUT" != "0" ] && command -v timeout >/dev/null 2>&1; then
  runner=(timeout "$TIMEOUT")
fi

echo "running ${#binaries[@]} binaries from $DIR"
echo

failed=()
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

for path in "${binaries[@]}"; do
  name="$(basename "$path")"
  printf '%-24s ' "$name"
  start="$(now_ms)"
  "${runner[@]}" "$path" ${PASS_ARGS+"${PASS_ARGS[@]}"} >"$log" 2>&1
  status=$?
  elapsed=$(( $(now_ms) - start ))

  # A gtest verdict makes the rest of the output noise. Without one the binary is a
  # benchmark or a tool, and what it printed is the whole point of running it.
  summary="$(grep -E '^\[  (PASSED|FAILED)  \]' "$log" | tail -1)"
  is_report=0
  if [ -z "$summary" ]; then
    summary="exit $status"
    is_report=1
  fi

  if [ $status -eq 0 ]; then
    printf '%sok%s   %-38s %s%s ms%s\n' "$GREEN" "$RESET" "$summary" "$DIM" "$elapsed" "$RESET"
  else
    [ $status -eq 124 ] && summary="timed out after ${TIMEOUT}s"
    printf '%sFAIL%s %-38s %s%s ms%s\n' "$RED" "$RESET" "$summary" "$DIM" "$elapsed" "$RESET"
    failed+=("$name")
  fi

  if { [ $VERBOSE -eq 1 ] || [ $status -ne 0 ] ||
       { [ $is_report -eq 1 ] && [ $QUIET -eq 0 ]; }; } && [ -s "$log" ]; then
    sed 's/^/    /' "$log"
    echo
  fi
done

echo
if [ ${#failed[@]} -eq 0 ]; then
  printf '%sall %d passed%s\n' "$GREEN" "${#binaries[@]}" "$RESET"
  exit 0
fi
printf '%s%d of %d failed:%s %s\n' "$RED" "${#failed[@]}" "${#binaries[@]}" "$RESET" "${failed[*]}"
exit 1
