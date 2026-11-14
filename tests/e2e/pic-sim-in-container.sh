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
# The full tier M feature set, the same the P14 canary compiles for this
# part, plus the limits the generated header names.
defines=(-DR2S_WITH_CODEC -DR2S_MAX_NEST="${R2S_MAX_NEST_DEF:?}"
  -DR2S_MAX_FRAME="${R2S_MAX_FRAME_DEF:?}" -DR2S_WITH_LOG -DR2S_WITH_TIME)

hex=$work/pic18f4550.hex
if ! "$epic_cc" --target p18f4550 "${flags[@]}" "${defines[@]}" \
  "${sources[@]}" -o "$hex" >"$work/build.log" 2>&1; then
  echo "pic-sim: epic-cc failed" >&2
  cat "$work/build.log" >&2
  exit 1
fi
grep -E 'flash:|RAM:' "$work/build.log" | sed 's/^/pic-sim: /'

# The SCL script the simulator runs: one process opens the frame list and
# hands each line to the receive register, which is how MPLAB SIM feeds a
# UART under a script. packetin reads hex, so every byte arrives as the
# transcript wrote it, a frame's 0x00 delimiter included.
cat >"$work/stimulus.scl" <<'SCL'
testbench for "PIC18F4550" is
begin
  process is
    file InFile : text;
    variable Status : file_open_status;
    variable InLine : line;
  begin
    file_open(Status, InFile, "rx.txt", read_mode);
    if Status != open_ok then
      report "cannot open rx.txt" severity failure;
    end if;
    while endfile(InFile) == false loop
      readline(InFile, InLine);
      packetin(InLine, RCREG, true);
    end loop;
    wait;
  end process;
end testbench;
SCL

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
