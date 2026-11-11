#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Compile the core and the static C++ sketch with epic-cc for two PIC parts.
# The bundle lives outside the dev image: the pinned release downloads into
# the cache, and a run without the toolchain skips instead of failing, so
# the advisory job stays cheap. R2S_EPIC_CC_BIN points at an installed
# compiler instead of downloading (tests and preinstalled toolchains).
# Known epic-cc gaps live in ci/pic-canary-baseline.txt: a recorded
# signature warns, a new one fails, and a stale entry fails to shrink.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/pic-canary
# One lock for the canary build directory, held for the whole run: a direct
# run and a suite run of one tree must not write the same hex at once.
if [ -z "${R2S_PIC_CANARY_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_PIC_CANARY_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/scripts/build-pic-canary.sh" "$@"
fi
mkdir -p "$build" || exit 1

ver=$(grep '^EPIC_CC_VERSION=' "$root/ci/versions.env" | cut -d= -f2)
sha=$(grep '^EPIC_CC_SHA256=' "$root/ci/versions.env" | cut -d= -f2)
[ -n "$ver" ] && [ -n "$sha" ] || {
  echo "build-pic-canary: ci/versions.env names no EPIC_CC_VERSION or EPIC_CC_SHA256" >&2
  exit 1
}
archive=epic-cc-$ver-x86_64-linux
url="https://github.com/apojomovsky/epic-cc/releases/download/v$ver/$archive.zip"

epic_cc=${R2S_EPIC_CC_BIN:-}
cache=${R2S_CACHE_DIR:-$HOME/.cache/ros2serial}/epic-cc
if [ -n "$epic_cc" ]; then
  [ -x "$epic_cc" ] || {
    echo "build-pic-canary: R2S_EPIC_CC_BIN names no executable ($epic_cc)" >&2
    exit 1
  }
else
  # The bundle carries its own clang, so the cache holds one directory
  # per pinned version and never touches the image.
  epic_cc="$cache/$archive/epic-cc"
  if [ ! -x "$epic_cc" ]; then
    if ! command -v curl >/dev/null 2>&1; then
      echo "pic-canary: no epic-cc and no downloader; skipping"
      exit 0
    fi
    mkdir -p "$cache" || exit 1
    if ! curl -fsSL -o "$cache/$archive.zip" "$url"; then
      echo "build-pic-canary: cannot fetch $url" >&2
      exit 1
    fi
    # A hash mismatch is a wrong bundle, not an absent one, so it fails.
    (cd "$cache" && echo "$sha  $archive.zip" | sha256sum -c -) || exit 1
    python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
      "$cache/$archive.zip" "$cache" || exit 1
    # The zip carries no permission bits, so the driver and its clang
    # helpers land non-executable and the compile fails with denied.
    chmod +x "$epic_cc" "$cache/$archive/clang/bin/"* || exit 1
    [ -x "$epic_cc" ] || {
      echo "build-pic-canary: $url holds no epic-cc binary" >&2
      exit 1
    }
  fi
fi

# The whole core except the host test reset, which firmware never links.
# The bundled descriptor tables join it: the checked-in golden copy the
# generator tests pin, the same file the AVR syntax check stands in.
sources=()
for src in "$root"/core/src/*.c; do
  case "$src" in *test_reset.c) continue ;; esac
  sources+=("$src")
done
sources+=("$root/gen/tests/golden/bundle/r2s_bundled.c")
[ "${#sources[@]}" -gt 0 ] || {
  echo "build-pic-canary: no core sources under $root/core/src" >&2
  exit 1
}
# The static firmware config the C++ tests build, so the canary differs
# from a proven build only in the toolchain and the target.
flags=(-I "$root/core/include" -I "$root/cpp/include")
# epic-cc drives its clang with -nostdinc, which also drops the compiler's
# own resource headers, so float.h is missing from the search path. The
# bundle ships them beside the driver, so they rejoin here; a preinstalled
# compiler outside the bundle layout skips this and stands as it is.
for resdir in "$cache/$archive/clang/lib/clang/"*/include; do
  [ -d "$resdir" ] && flags+=(-I "$resdir")
done
defines=(-DR2S_WITH_CODEC -DR2S_MAX_NEST=4 -DR2S_WITH_LOG -DR2S_WITH_TIME)

# The port stubs answer from constants: the canary links, never runs.
cat >"$build/canary_port.c" <<'EOF'
#include "r2s/r2s_port.h"

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    (void)buf;
    (void)max;
    return 0;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
}

uint16_t r2s_port_tx_free(void) { return 0xFFFF; }

uint32_t r2s_port_tick(void) { return 0; }
EOF
# The C canary owns a topic list in the C spelling, proving the core and
# the static dispatch link without any C++.
cat >"$build/canary_c.c" <<'EOF'
#define R2S_NODE_NAME "canary"
#define R2S_TOPICS(X) \
    X(Publisher, std_msgs__msg__Float32, battery_pub, "battery") \
    X(Subscription, geometry_msgs__msg__Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)
#include <ros2serial.h>

void on_cmd_vel(const geometry_msgs__msg__Twist *msg) { (void)msg; }

void main(void)
{
    std_msgs__msg__Float32 battery;

    if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return;
    }
    r2s_init();
    battery.data = 0.0f;
    (void)r2s_publish_battery_pub(&battery);
    r2s_spin_some();
}
EOF
# The static sketch from the ticket's 1-to-1 map, with a main instead of
# the Arduino setup and loop, proving the static path links. Global
# constructors run the publisher and subscription objects, as on a board.
cat >"$build/canary_static.cpp" <<'EOF'
#define R2S_TOPICS(X) \
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)
#include <ros2serial.h>
#include "r2s/r2s.h"

void on_cmd_vel(const geometry_msgs::msg::Twist &msg) { (void)msg; }

r2s::Node node("canary");

struct canary_port {
    void begin(unsigned long baud) { (void)baud; }
};

void main(void)
{
    canary_port port;
    std_msgs::msg::Float32 battery;

    node.begin(port, 115200UL);
    battery.data = 0.0f;
    (void)battery_pub.publish(battery);
    node.spin_some();
}
EOF

status=0
gaps="$build/known_gaps.log"
observed="$build/observed_gaps.log"
new_gaps="$build/new_gaps.log"
stale_gaps="$build/stale_gaps.log"
: >"$gaps" || exit 1
: >"$observed" || exit 1
: >"$new_gaps" || exit 1
: >"$stale_gaps" || exit 1

# The recorded gaps live in ci/pic-canary-baseline.txt, one line per
# target and label. A missing or malformed file is wiring, not a gap.
baseline=${R2S_PIC_CANARY_BASELINE:-"$root/ci/pic-canary-baseline.txt"}
[ -f "$baseline" ] || {
  echo "build-pic-canary: no baseline ($baseline)" >&2
  exit 1
}
trim() {
  printf '%s\n' "$1" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
}
# The message without its locations: file:line tokens, bare line and
# column numbers and empty brackets go, the wording itself stays exact.
normalize_sig() {
  printf '%s\n' "$1" |
    sed -E -e 's/\x1b\[[0-9;]*[A-Za-z]//g' \
      -e 's/[A-Za-z0-9_.\/+~-]+\.(c|h|cpp|hpp|cc|cxx):[0-9]+(:[0-9]+)?//g' \
      -e 's/(at |in )?lines? [0-9]+(-[0-9]+)?//g' \
      -e 's/(at |in )?col(umn)?s? [0-9]+//g' \
      -e 's/\[\]//g' -e 's/\(\)//g' \
      -e 's/[[:space:]][[:space:]]*/ /g' -e 's/^ //;s/ $//'
}
norm_baseline="$build/baseline.normalized"
: >"$norm_baseline" || exit 1
while IFS= read -r line; do
  line=${line%$'\r'}
  line=$(trim "$line")
  case "$line" in ''|'#'*) continue ;; esac
  pipes=${line//[^|]/}
  IFS='|' read -r b_target b_label b_sig b_extra <<<"$line"
  b_target=$(trim "$b_target")
  b_label=$(trim "$b_label")
  b_sig=$(trim "$b_sig")
  b_extra=$(trim "${b_extra:-}")
  [ -n "$b_target" ] && [ -n "$b_label" ] && [ -n "$b_sig" ] && [ -z "$b_extra" ] && [ "${#pipes}" = 2 ] || {
    echo "build-pic-canary: bad baseline line (want 'target | label | signature'): $line" >&2
    exit 1
  }
  printf '%s | %s | %s\n' "$b_target" "$b_label" "$(normalize_sig "$b_sig")" >>"$norm_baseline" || exit 1
done <"$baseline"

# One whole-program compile. An internal compiler error the baseline
# records is the known toolchain gap: it warns and stays green. One it
# never recorded is a new break and fails, naming target and signature.
# Any other failure is ours (wiring or a real PIC break) and fails.
compile_one() {
  local target=$1 label=$2 out
  shift 2
  if out=$("$epic_cc" --target "$target" "${flags[@]}" "${defines[@]}" "$@" 2>&1); then
    printf '%s\n' "$out"
  else
    printf '%s\n' "$out"
    case "$out" in
    *"internal compiler error"*)
      detail=$(printf '%s\n' "$out" | grep "internal compiler error" | head -n 1)
      detail=${detail#*internal compiler error: }
      key=$(printf '%s | %s | %s\n' "$target" "$label" "$(normalize_sig "$detail")")
      if grep -qxF -- "$key" "$norm_baseline"; then
        echo "::warning::pic-canary: known epic-cc gap on $target ($label): $detail"
        echo "$target ($label): $detail" >>"$gaps" || exit 1
        printf '%s\n' "$key" >>"$observed" || exit 1
      else
        echo "::error::pic-canary: new epic-cc failure on $target ($label): $detail" >&2
        echo "pic-canary: append '$key' to ci/pic-canary-baseline.txt and check it in" >&2
        printf '%s\n' "$key" >>"$new_gaps" || exit 1
        status=1
      fi
      ;;
    *)
      echo "pic-canary: $label failed for $target" >&2
      status=1
      ;;
    esac
  fi
}
for target in p16f1938 p18f4550; do
  compile_one "$target" "core C sources" \
    "${sources[@]}" "$build/canary_port.c" "$build/canary_c.c" \
    -o "$build/canary_c_$target.hex"
  compile_one "$target" "static C++ sketch" \
    "${sources[@]}" "$build/canary_port.c" "$build/canary_static.cpp" \
    -o "$build/canary_static_$target.hex"
done
# A baseline entry that stops reproducing is a closed gap: it fails and
# asks for the file to shrink, so the improvement never hides silently.
while IFS= read -r want; do
  [ -n "$want" ] || continue
  if ! grep -qxF -- "$want" "$observed"; then
    echo "pic-canary: baseline entry stops reproducing, remove it: $want" >&2
    echo "pic-canary: remove that line from ci/pic-canary-baseline.txt and check it in" >&2
    printf '%s\n' "$want" >>"$stale_gaps" || exit 1
    status=1
  fi
done <"$norm_baseline"
# Every gap rides the job summary, so the reader sees the state without
# opening the log; the hex report below lists only what compiled.
{
  if [ -s "$gaps" ]; then
    echo "### PIC canary: known epic-cc gaps"
    echo ""
    echo "Advisory; tracked for #148. The toolchain, not the core, fails these."
    echo ""
    cat "$gaps"
    echo ""
  fi
  if [ -s "$new_gaps" ]; then
    echo "### PIC canary: new epic-cc failures"
    echo ""
    cat "$new_gaps"
    echo ""
  fi
  if [ -s "$stale_gaps" ]; then
    echo "### PIC canary: baseline entries that stop reproducing"
    echo ""
    cat "$stale_gaps"
    echo ""
  fi
} >>"${GITHUB_STEP_SUMMARY:-/dev/null}"
while IFS= read -r gap; do
  echo "pic-canary: known gap: $gap (advisory)"
done <"$gaps"
for hex in "$build"/canary_*_p16f1938.hex "$build"/canary_*_p18f4550.hex; do
  [ -e "$hex" ] || continue
  echo "pic-canary: $(basename "$hex") $(wc -c <"$hex") bytes"
done
[ "$status" = 0 ] || exit 1
