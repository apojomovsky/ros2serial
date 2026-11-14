#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# PIC18F4550 in MPLAB SIM, inside the simulator image: compile the firmware
# with the pinned epic-cc, feed the vector's host frames to its UART, and
# compare every byte it sends back to the vector. Called by
# tests/e2e/run-pic-sim.sh, which prepares the generated pair and the frame
# lists first; running it directly needs those already under /build/pic-sim.
set -uo pipefail
root=${R2S_PIC_SIM_SRC:-/src}
build=${R2S_PIC_SIM_BUILD:-/build}
cd "$root" || exit 2
work=$build/run
[ -d "$work/gen" ] || {
  echo "pic-sim: no generated pair under $work; run make pic-sim" >&2
  exit 1
}

mdb=${R2S_MDB_BIN:-/opt/microchip/mplabx/v6.35/mplab_platform/bin/mdb.sh}
[ -x "$mdb" ] || {
  echo "pic-sim: no mdb at $mdb: the image does not carry MPLAB SIM" >&2
  exit 1
}

ver=$(sed -n 's/^EPIC_CC_VERSION=//p' "$root/ci/versions.env" | tail -1)
sha=$(sed -n 's/^EPIC_CC_SHA256=//p' "$root/ci/versions.env" | tail -1)
[ -n "$ver" ] && [ -n "$sha" ] || {
  echo "pic-sim: ci/versions.env must pin EPIC_CC_VERSION and EPIC_CC_SHA256" >&2
  exit 1
}

# The compiler bundle downloads into the build directory, never into the
# image: the image carries the simulator, the pin here carries the compiler.
mkdir -p "$build/epic-cc" || exit 1
cache=$build/epic-cc/epic-cc-$ver-x86_64-linux
epic_cc=$cache/epic-cc
if [ ! -x "$epic_cc" ]; then
  zip=$build/epic-cc/epic-cc-$ver-x86_64-linux.zip
  url="https://github.com/apojomovsky/epic-cc/releases/download/v$ver/epic-cc-$ver-x86_64-linux.zip"
  curl -fsSL "$url" -o "$zip" || {
    echo "pic-sim: cannot download $url" >&2
    exit 1
  }
  got=$(sha256sum "$zip" | awk '{ print $1 }')
  [ "$got" = "$sha" ] || {
    echo "pic-sim: epic-cc $ver hashes to $got, ci/versions.env pins $sha" >&2
    exit 1
  }
  unzip -q -o "$zip" -d "$build/epic-cc" || exit 1
fi
[ -x "$epic_cc" ] || {
  echo "pic-sim: no epic-cc at $epic_cc" >&2
  exit 1
}

# The clock the firmware runs at, read from its source so the simulator's
# oscillator cannot drift from the divisor the port programs.
clk=$(sed -n 's/^#define R2S_PIC18F4550_CLK_HZ[ \t]*\([0-9]\+\)u$/\1/p' \
  "$root/ports/epic-hal/r2s_pic18f4550_firmware.c")
[ -n "$clk" ] || {
  echo "pic-sim: the firmware names no clock" >&2
  exit 1
}

sources=()
for src in "$root"/core/src/*.c; do
  case "$src" in
  */test_reset.c) continue ;;
  esac
  sources+=("$src")
done
sources+=("$work/gen/r2s_generated.c"
  "$root/ports/epic-hal/r2s_pic18f4550_port.c"
  "$root/ports/epic-hal/r2s_pic18f4550_firmware.c")

# epic-cc drives its clang with -nostdinc, which also drops the compiler's
# resource headers, so float.h leaves the search path. The bundle ships
# them beside the driver, so they rejoin here.
flags=(-I "$root/core/include" -I "$work/gen" -I "$root/ports/epic-hal")
for resdir in "$(dirname "$epic_cc")"/clang/lib/clang/*/include; do
  [ -d "$resdir" ] && flags+=(-I "$resdir")
done
# The feature set the frozen transcript was recorded with, so the HELLO
# compares field for field: the replay build in core/tests/CMakeLists.txt
# turns logging and time off and fixes the tick at 1 MHz, and the vector's
# HELLO carries features 0x00 and tick_hz 1000000. The limits come from the
# generated header, which ports/epic-hal/r2s.yaml pins to the vector's 512.
defines=(-DR2S_WITH_CODEC -DR2S_MAX_NEST="${R2S_MAX_NEST_DEF:?}"
  -DR2S_MAX_FRAME="${R2S_MAX_FRAME_DEF:?}" -DR2S_TICK_HZ=1000000)

hex=$work/pic18f4550.hex
baseline=$root/ci/pic-sim-baseline.txt
[ -f "$baseline" ] || {
  echo "pic-sim: no baseline at $baseline" >&2
  exit 1
}

# The message without its volatile parts, the same normalization the P14
# canary uses: locations stripped, wording kept exact.
normalize_sig() {
  printf '%s\n' "$1" |
    sed -E -e 's/\x1b\[[0-9;]*[A-Za-z]//g' \
      -e 's/[A-Za-z0-9_.\/+~-]+\.(c|h|cpp|hpp|cc|cxx):[0-9]+(:[0-9]+)?//g' \
      -e 's/(at |in )?lines? [0-9]+(-[0-9]+)?//g' \
      -e 's/(at |in )?col(umn)?s? [0-9]+//g' \
      -e 's/"[0-9]+"/""/g' \
      -e 's/\(([0-9]+),/(,/g' -e 's/, ([0-9]+)\)/, )/g' \
      -e 's/::[0-9]+//g' \
      -e 's/\[\]//g' -e 's/\(\)//g' \
      -e 's/[[:space:]][[:space:]]*/ /g' -e 's/^ //;s/ $//'
}

norm_baseline=$work/baseline.normalized
: >"$norm_baseline" || exit 1
while IFS= read -r line; do
  line=${line%$'\r'}
  case "$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')" in
  ''|'#'*) continue ;;
  esac
  IFS='|' read -r b_label b_sig b_extra <<<"$line"
  b_label=$(printf '%s' "$b_label" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
  b_sig=$(printf '%s' "$b_sig" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
  b_extra=$(printf '%s' "${b_extra:-}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
  if [ -z "$b_label" ] || [ -z "$b_sig" ] || [ -n "$b_extra" ]; then
    echo "pic-sim: bad baseline line (want 'label | signature'): $line" >&2
    exit 1
  fi
  printf '%s | %s\n' "$b_label" "$(normalize_sig "$b_sig")" >>"$norm_baseline" || exit 1
done <"$baseline"

status=0
observed=$work/observed_gaps.log
new=$work/new_gaps.log
: >"$observed" && : >"$new" || exit 1

# One compile. An internal compiler error the baseline records is the known
# toolchain gap: it warns and the run goes on to the simulator. One it never
# recorded is a new break and fails. Any other failure is ours and fails.
# A gap that stops reproducing fails below, so the file shrinks instead of
# hiding the fix.
compile() {
  local out
  if out=$("$epic_cc" --target p18f4550 "${flags[@]}" "${defines[@]}" \
    "${sources[@]}" -o "$hex" 2>&1); then
    printf '%s\n' "$out" | tee "$work/build.log"
    return 0
  fi
  printf '%s\n' "$out" | tee "$work/build.log"
  case "$out" in
  *"internal compiler error"*)
    local detail key
    detail=$(printf '%s\n' "$out" | grep "internal compiler error" | head -n 1)
    detail=${detail#*internal compiler error: }
    key="pic18f4550 firmware | $(normalize_sig "$detail")"
    if grep -qxF -- "$key" "$norm_baseline"; then
      echo "::warning::pic-sim: known epic-cc gap: $detail"
      printf '%s\n' "$key" >>"$observed" || exit 1
      return 0
    fi
    echo "pic-sim: new epic-cc failure: $detail" >&2
    echo "pic-sim: append '$key' to ci/pic-sim-baseline.txt and check it in" >&2
    printf '%s\n' "$key" >>"$new" || exit 1
    return 1
    ;;
  *)
    echo "pic-sim: the compile failed" >&2
    return 1
    ;;
  esac
}
compile || status=1
grep -E 'flash:|RAM:' "$work/build.log" 2>/dev/null | sed 's/^/pic-sim: /' || true
while IFS= read -r gap; do
  [ -n "$gap" ] && echo "::warning::pic-sim: known gap: $gap (advisory)"
done <"$observed"
[ -s "$new" ] && cat "$new" >&2

# A baseline entry that stops reproducing is a closed gap: it fails and asks
# for the file to shrink, so the improvement never hides silently.
while IFS= read -r want; do
  [ -n "$want" ] || continue
  if ! grep -qxF -- "$want" "$observed"; then
    echo "pic-sim: baseline entry stops reproducing, remove it: $want" >&2
    status=1
  fi
done <"$norm_baseline"

if [ "$status" -ne 0 ]; then
  exit 1
fi
# Only a real hex can be simulated. A known gap leaves none, so the run
# reports the gap and stops green here rather than failing on a missing
# file, and the nightly keeps showing the epic-cc state.
if [ ! -f "$hex" ]; then
  echo "pic-sim: skipped the simulator run: the compile is a known epic-cc gap"
  exit 0
fi

# The SCL script and the frame list come from tools/pic-sim-frames.py,
# which ran in the dev image before this one: it computes each gap and
# writes a plain `wait for N ms;`, because the simulator's SCL subset
# rejects time arithmetic. The frames are decimal because packetin parses
# the rest of the line as decimals, so every byte arrives as written, a
# 0x00 delimiter included.
[ -s "$work/stimulus.scl" ] || {
  echo "pic-sim: no stimulus script at $work/stimulus.scl" >&2
  exit 1
}

capture=$work/uart.bin
rm -f "$capture"
# The capture arms before the program loads and the stimulus attaches
# after it: those two orders are the ones the simulator honors, and the
# reverse of either leaves an empty capture.
cat >"$work/mdb.txt" <<MDB
device PIC18F4550
set oscillator.frequency $clk
set uart1io.uartioenabled true
set uart1io.output file
set uart1io.outputfile $capture
hwtool SIM
program $hex
stim stimulus.scl
run
wait 3000
halt
quit
MDB

# The stimulus file resolves relative to the script's directory, so the run
# happens there.
if ! (cd "$work" && "$mdb" "$work/mdb.txt" >"$work/mdb.log" 2>&1); then
  echo "pic-sim: the simulator exited non-zero" >&2
  tail -40 "$work/mdb.log" >&2
  exit 1
fi
if grep -qiE 'E0101-SIM|E0103-SIM|E98[0-9]+-SCL' "$work/mdb.log"; then
  echo "pic-sim: the simulator rejected the stimulus script" >&2
  grep -iE 'E0101-SIM|E0103-SIM|E98[0-9]+-SCL' "$work/mdb.log" >&2
  exit 1
fi
[ -s "$capture" ] || {
  echo "pic-sim: the firmware put no bytes on the UART" >&2
  tail -40 "$work/mdb.log" >&2
  exit 1
}

got=$(od -An -tx1 "$capture" | tr -d ' \n')
# Every MCU frame the transcript carries, in order: the HELLO, the two
# announcements, the heartbeats. The firmware sends exactly those, so the
# capture must open with them byte for byte.
want=$(tr -d '\n' <"$work/want_mcu.txt")
case "$got" in
"$want"*) ;;
*)
  echo "pic-sim: the replies do not match the vector" >&2
  echo "  want: $want" >&2
  echo "  got:  ${got:0:${#want}}" >&2
  diff <(printf '%s\n' "$want" | fold -w2) \
    <(printf '%s\n' "${got:0:${#want}}" | fold -w2) | head -20 >&2
  exit 1
  ;;
esac
echo "pic-sim: ${#want} hex digits of MCU frames match spec/vectors/session/pub_sub_handshake.yaml"
