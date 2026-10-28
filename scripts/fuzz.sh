#!/usr/bin/env bash
# Build the libFuzzer targets and run each over its seed corpus, then fuzz it.
# Usage: fuzz.sh; env TARGET picks one target (framer, rcobs_decode, session,
# codec), default all four; env R2S_FUZZ_SECONDS sets the fuzz time per target,
# default 60; 0 only runs the seeds. Exits 1 on any build, seed or crash.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/fuzz
targets=(${TARGET:-framer rcobs_decode session codec})
seconds=${R2S_FUZZ_SECONDS:-60}

# A stale seed next to a changed vector must fail here, not fuzz silently.
python3 tools/fuzz_corpus.py --check || exit 1

mkdir -p "$build" || exit 1
failed=()
for target in "${targets[@]}"; do
  # Each target links the core files it exercises: the port stubs each
  # target file defines are exactly the ones its set needs.
  case "$target" in
    framer) sources="core/src/framer.c core/src/rcobs.c core/src/crc.c" ;;
    rcobs_decode) sources="core/src/rcobs.c" ;;
    session) sources="core/src/*.c" ;;
    codec) sources="core/src/codec.c core/src/tx.c core/src/rcobs.c core/src/crc.c core/src/varint.c" ;;
    *)
      echo "unknown fuzz target '$target'"
      failed+=("$target")
      continue
      ;;
  esac
  source="core/fuzz/fuzz_$target.c"
  binary="$build/fuzz_$target"
  echo "-- fuzz: $target"
  # The core's own flags (core/CMakeLists.txt), so the targets meet the same
  # warnings gate, plus the libFuzzer and sanitizer runtime they run under.
  # shellcheck disable=SC2086 # sources holds several files.
  clang -std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow \
    -Wstrict-prototypes -Wmissing-prototypes -Wundef \
    -DR2S_TESTING -DR2S_MAX_FRAME=512 -DR2S_WITH_CODEC -DR2S_MAX_NEST=2 -DR2S_WITH_LOG \
    -I core/include -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -g -O1 $sources "$source" -o "$binary" || {
    echo "-- fuzz: $target build FAILED"
    failed+=("$target")
    continue
  }
  # Fuzzing writes new findings back into its corpus directory, so each target
  # runs on a scratch copy: the committed seeds stay byte-identical while
  # libFuzzer still runs every seed before it starts mutating.
  work=$(mktemp -d) || exit 1
  cp "core/fuzz/corpus/$target"/* "$work"/
  ok=1
  if [ "$seconds" = 0 ]; then
    "$binary" -runs=0 -timeout=30 "$work" || {
      echo "-- fuzz: $target seed FAILED"
      failed+=("$target")
      ok=0
    }
  else
    "$binary" -max_total_time="$seconds" -timeout=30 -print_final_stats=1 "$work" || {
      echo "-- fuzz: $target fuzz FAILED"
      failed+=("$target")
      ok=0
    }
  fi
  rm -rf "$work"
  [ "$ok" = 1 ] && echo "-- fuzz: $target ok"
done
if [ "${#failed[@]}" -gt 0 ]; then
  echo "fuzz targets failed: ${failed[*]}"
  exit 1
fi
