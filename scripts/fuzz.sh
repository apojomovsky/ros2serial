#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Build the libFuzzer targets and run each over its seed corpus, then fuzz it.
# Usage: fuzz.sh; env TARGET picks one target (framer, rcobs_decode, session,
# codec), default all four; env R2S_FUZZ_SECONDS sets the fuzz time per target,
# default 60; 0 only runs the seeds. R2S_FUZZ_COVERAGE=1 additionally replays
# the committed seeds under gcov and renders tools/fuzz_coverage.py's report
# (report only, never a gate). R2S_FUZZ_ARTIFACTS_DIR keeps whatever libFuzzer
# adds beyond the copied seeds (crashers and new inputs) under that directory
# for upload; without it the scratch copy is deleted. Exits 1 on any build,
# seed or crash.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/fuzz
targets=(${TARGET:-framer rcobs_decode session codec})
seconds=${R2S_FUZZ_SECONDS:-60}
coverage=${R2S_FUZZ_COVERAGE:-0}
artifacts=${R2S_FUZZ_ARTIFACTS_DIR:-}

# A stale seed next to a changed vector must fail here, not fuzz silently.
python3 tools/fuzz_corpus.py --check || exit 1

mkdir -p "$build" || exit 1
failed=()
# The probe first: the fuzzer accepts a refusal, so a table or seed header
# that decodes to nothing would pass every target silently.
probe="$build/seed_probe"
# shellcheck disable=SC2086 # sources holds several files.
codec_sources="core/src/codec.c core/src/tx.c core/src/rcobs.c core/src/crc.c core/src/varint.c"
clang -std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow \
  -Wstrict-prototypes -Wmissing-prototypes -Wundef \
  -DR2S_TESTING -DR2S_MAX_FRAME=512 -DR2S_WITH_CODEC -DR2S_MAX_NEST=2 -DR2S_WITH_LOG -DR2S_WITH_TIME \
  -I core/include -fsanitize=address,undefined -fno-sanitize-recover=all \
  -fno-omit-frame-pointer -g -O1 core/fuzz/seed_probe.c $codec_sources -o "$probe" || exit 1
"$probe" core/fuzz/corpus/codec/* || exit 1
# Each target links the core files it exercises: the port stubs each target
# file defines are exactly the ones its set needs. The coverage step reuses
# the same sets, so one function names them.
target_sources() {
  case "$1" in
    framer) echo "core/src/framer.c core/src/rcobs.c core/src/crc.c" ;;
    rcobs_decode) echo "core/src/rcobs.c" ;;
    session) echo "core/src/"*.c ;;
    codec) echo "$codec_sources" ;;
    *)
      echo "unknown fuzz target '$1'"
      return 1
      ;;
  esac
}
for target in "${targets[@]}"; do
  sources=$(target_sources "$target") || {
    failed+=("$target")
    continue
  }
  source="core/fuzz/fuzz_$target.c"
  binary="$build/fuzz_$target"
  echo "-- fuzz: $target"
  # The core's own flags (core/CMakeLists.txt), so the targets meet the same
  # warnings gate, plus the libFuzzer and sanitizer runtime they run under.
  # shellcheck disable=SC2086 # sources holds several files.
  clang -std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow \
    -Wstrict-prototypes -Wmissing-prototypes -Wundef \
    -DR2S_TESTING -DR2S_MAX_FRAME=512 -DR2S_WITH_CODEC -DR2S_MAX_NEST=2 -DR2S_WITH_LOG -DR2S_WITH_TIME \
    -I core/include -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -g -O1 $sources "$source" -o "$binary" || {
    echo "-- fuzz: $target build FAILED"
    failed+=("$target")
    continue
  }
  # Fuzzing writes new findings back into its corpus directory, so each target
  # runs on a scratch copy: the committed seeds stay byte-identical while
  # libFuzzer still runs every seed before it starts mutating. Crashers land
  # in the scratch copy too, never in the checkout.
  work=$(mktemp -d) || exit 1
  cp "core/fuzz/corpus/$target"/* "$work"/
  ok=1
  if [ "$seconds" = 0 ]; then
    "$binary" -runs=0 -timeout=30 -artifact_prefix="$work/" "$work" || {
      echo "-- fuzz: $target seed FAILED"
      failed+=("$target")
      ok=0
    }
  else
    "$binary" -max_total_time="$seconds" -timeout=30 -print_final_stats=1 -artifact_prefix="$work/" "$work" || {
      echo "-- fuzz: $target fuzz FAILED"
      failed+=("$target")
      ok=0
    }
  fi
  # Whatever libFuzzer added beyond the copied seeds (crashers and new
  # inputs) is only kept when the caller names a directory for it: the
  # nightly uploads it, and a regression becomes a permanent seed when its
  # author copies it to core/fuzz/corpus/<target>/regression_*.
  if [ -n "$artifacts" ]; then
    mkdir -p "$artifacts/$target" || exit 1
    for entry in "$work"/*; do
      name=${entry##*/}
      [ -e "core/fuzz/corpus/$target/$name" ] || cp "$entry" "$artifacts/$target/$name"
    done
    kept=$(ls "$artifacts/$target" | wc -l)
    echo "-- fuzz: $target kept $kept new inputs in $artifacts/$target"
  fi
  rm -rf "$work"
  [ "$ok" = 1 ] && echo "-- fuzz: $target ok"
done
# Seed-corpus coverage, nightly only: replay the committed seeds under gcov
# and report which lines each target reaches. A low percentage never fails
# here; only a broken measurement does.
if [ "$coverage" = 1 ]; then
  command -v gcovr >/dev/null 2>&1 || { echo "gcovr not found: run it in the dev image" >&2; exit 1; }
  mkdir -p "$build/cov" || exit 1
  pairs=()
  for target in "${targets[@]}"; do
    sources=$(target_sources "$target") || { failed+=("$target-coverage"); continue; }
    obj="$build/cov/$target/obj"
    # Fresh objects every run: a changed flag set would otherwise read old
    # notes against new counts and fail with a stamp mismatch.
    rm -rf "$obj"
    mkdir -p "$obj" || exit 1
    # shellcheck disable=SC2086 # sources holds several files.
    ok=1
    for src in $sources core/fuzz/fuzz_"$target".c core/fuzz/cov_driver.c; do
      # The objects carry absolute paths, so notes and counts land beside
      # them in $obj with no extra profile directory to confuse gcov.
      gcc -std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow \
        -Wstrict-prototypes -Wmissing-prototypes -Wundef \
        -DR2S_TESTING -DR2S_MAX_FRAME=512 -DR2S_WITH_CODEC -DR2S_MAX_NEST=2 -DR2S_WITH_LOG -DR2S_WITH_TIME \
        -I core/include --coverage -O0 -g -c "$src" -o "$obj/$(basename "$src" .c).o" || ok=0
    done
    [ "$ok" = 1 ] || { echo "-- fuzz: $target coverage build FAILED"; failed+=("$target-coverage"); continue; }
    # shellcheck disable=SC2046 # one object per compiled source.
    gcc --coverage $(echo "$obj"/*.o) -o "$obj/cov_$target" || {
      echo "-- fuzz: $target coverage link FAILED"; failed+=("$target-coverage"); continue
    }
    "$obj/cov_$target" "core/fuzz/corpus/$target"/* || {
      echo "-- fuzz: $target coverage replay FAILED"; failed+=("$target-coverage"); continue
    }
    # One ruler per call, as the coverage gate does: only core/src lines
    # count, so the driver and the target harness never pad the figure.
    gcovr --config /dev/null --root "$root" --filter 'core/src/' \
      --object-directory "$obj" --json --output "$build/cov/$target.json" || {
      echo "-- fuzz: $target coverage report FAILED"; failed+=("$target-coverage"); continue
    }
    pairs+=(--target "$target" --summary "$build/cov/$target.json")
    echo "-- fuzz: $target coverage ok"
  done
  if [ "${#pairs[@]}" -gt 0 ]; then
    python3 tools/fuzz_coverage.py "${pairs[@]}" --out "$build/fuzz-coverage.md" || {
      echo "fuzz coverage report FAILED"
      failed+=(coverage-report)
    }
  fi
  # The nightly uploads the report from the checkout, so a copy lands next
  # to the kept inputs whenever the caller names an artifacts directory.
  if [ -n "$artifacts" ]; then
    mkdir -p "$artifacts/coverage" || exit 1
    cp "$build/fuzz-coverage.md" "$build/cov/"*.json "$artifacts/coverage/" || {
      echo "fuzz coverage copy FAILED"
      failed+=(coverage-report)
    }
  fi
fi
if [ "${#failed[@]}" -gt 0 ]; then
  echo "fuzz targets failed: ${failed[*]}"
  exit 1
fi
