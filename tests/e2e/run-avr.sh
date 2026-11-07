#!/usr/bin/env bash
# The avr end-to-end suite: scenarios 1 to 4 against the slice sketch and
# scenario 12 against the flood sketch, on an emulated Uno and Mega, plus
# the baud matrix (the scenario 2 flow gates per baud per board) under the
# first RMW only. It stages the Arduino library at build time, compiles the
# sketches for both boards with burst-sized serial rings, and runs every
# scenario under each RMW. Builds go under R2S_BUILD_DIR, so the tree stays
# clean; the suite runs this script for E2E=avr. R2S_RMWS, R2S_E2E_BOARDS
# and R2S_E2E_BAUDS narrow their loops for a quicker iteration.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/e2e/avr
# One isolation slot per run: a leased DDS domain and Zenoh router port, so
# concurrent suites never share a domain. Held for the whole run, exactly
# like the build lock below.
if [ -z "${R2S_E2E_LEASED:-}" ]; then
  exec env R2S_E2E_LEASED=1 bash "$root/scripts/e2e-lease.sh" bash "$root/tests/e2e/run-avr.sh" "$@"
fi
# One lock for the e2e build directory, held for the entire run: a direct run
# and a suite run of one tree must not write the same binaries.
if [ -z "${R2S_E2E_AVR_LOCKED:-}" ]; then
  mkdir -p "$build" || exit 1
  exec env R2S_E2E_AVR_LOCKED=1 bash "$root/scripts/with-build-lock.sh" "$build/.lock" \
    bash "$root/tests/e2e/run-avr.sh" "$@"
fi
mkdir -p "$build" || exit 1

build_only=0
if [ "${1:-}" = --build-only ]; then build_only=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: tests/e2e/run-avr.sh [--build-only]" >&2; exit 2; }

# The harness binary the scenarios spawn: the simavr script builds it with
# --build-only and skips its own tests, which the suite runs separately.
bash tests/harness/simavr/run.sh --build-only || exit 1
sim_build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/harness/simavr
export R2S_SIMAVR_BIN=$sim_build/r2s_simavr

# The Arduino library is staged at build time: the bundle carries the sketch
# list's descriptors and the port glue, the core joins it here, so no bytes
# live twice in the repository and nothing can drift.
stage="$build/avr_slice"
rm -rf "$stage"
mkdir -p "$stage/src/r2s" || exit 1
PYTHONPATH="$root/gen" python3 -m r2s_gen.cli bundle --distro jazzy --out "$stage" || exit 1
cp core/include/r2s/*.h "$stage/src/r2s/" || exit 1
cp core/src/*.c "$stage/src/" || exit 1
rm -f "$stage/src/test_reset.c"
cp ports/arduino/r2s_arduino_port.cpp ports/arduino/r2s_arduino_port.h "$stage/src/" || exit 1
# The core never includes the bundled header, so the limits travel as -D
# flags read from it: the same set the cmake layout passes as definitions.
# A library sets none of its own flags on this toolchain, hence --build-property.
extras=$(PYTHONPATH="$root/gen" python3 - "$stage/src/r2s_bundled.h" <<'EOF'
import sys

from r2s_gen import layouts
from r2s_gen.msgs import MessageError

try:
    defines = layouts.build_defines(open(sys.argv[1]).read(), "src/r2s_bundled.h")
except (MessageError, OSError) as error:
    print(f"r2s-gen: error: {error}", file=sys.stderr)
    raise SystemExit(1)
print(" ".join("-D" + define for define in defines))
EOF
) || exit 1
# A flag string without the limits would compile a firmware that silently
# ignores every subscription or refuses its own table, so every value must
# be present and non-empty.
case "$extras" in
    *R2S_WITH_CODEC*R2S_MAX_FRAME=[0-9]*R2S_MAX_NEST=[0-9]*) ;;
    *)
        echo "run-avr: the extracted flags name no limits ($extras)" >&2
        exit 1
        ;;
esac
# Burst-sized rings (R10): 256 bytes is the practical choice, holding
# several slice frames in each direction, so a burst the slice can send
# neither overruns the RX ring nor stalls the loop on a full TX ring.
# One slice image per matrix baud: the sketch baud travels as R2S_BAUD, and
# 250000 stays out because the bridge has no termios constant for it (#426).
bauds=${R2S_E2E_BAUDS:-"115200 500000 1000000"}
for baud in $bauds; do
  case "$baud" in
    115200 | 500000 | 1000000) ;;
    *)
      echo "run-avr: unsupported baud $baud (want 115200, 500000 or 1000000)" >&2
      exit 1
      ;;
  esac
done
serial_flags="-DSERIAL_RX_BUFFER_SIZE=256 -DSERIAL_TX_BUFFER_SIZE=256"
mkdir -p "$build/fw" || exit 1
# Scenarios 1 to 4 run the default baud under its legacy file name, so the
# 115200 image builds even when the matrix narrows past it.
build_bauds=$bauds
case " $build_bauds " in
  *" 115200 "*) ;;
  *) build_bauds="115200 $build_bauds" ;;
esac
for baud in $build_bauds; do
  for board in uno mega; do
    mcu=atmega328p
    [ "$board" = mega ] && mcu=atmega2560
    out="$build/fw/$board-$baud"
    arduino-cli compile --fqbn "arduino:avr:$board" \
      --build-property "compiler.c.extra_flags=$extras" \
      --build-property "compiler.cpp.extra_flags=$serial_flags -DR2S_BAUD=$baud" \
      --library "$stage" --output-dir "$out" tests/e2e/avr_slice || exit 1
    avr-size "$out/avr_slice.ino.elf" || exit 1
    mv "$out/avr_slice.ino.elf" "$build/fw/slice-$mcu-$baud.elf" || exit 1
    # Scenarios 1 to 4 run the default baud under its legacy file name.
    if [ "$baud" = 115200 ]; then
      cp "$build/fw/slice-$mcu-$baud.elf" "$build/fw/slice-$mcu.elf" || exit 1
    fi
  done
done
# The flood sketch is scenario 12's alone: its per-tick TX budget lives in
# the test-only port hook, armed by the sketch, so the slice build above
# stays exactly what scenarios 1 to 11 run.
for board in uno mega; do
  mcu=atmega328p
  [ "$board" = mega ] && mcu=atmega2560
  out="$build/fw/$board-flood"
  arduino-cli compile --fqbn "arduino:avr:$board" \
    --build-property "compiler.c.extra_flags=$extras" \
    --build-property "compiler.cpp.extra_flags=$serial_flags -DR2S_TEST_TX_CAP=16" \
    --library "$stage" --output-dir "$out" tests/e2e/avr_flood || exit 1
  avr-size "$out/avr_flood.ino.elf" || exit 1
  mv "$out/avr_flood.ino.elf" "$build/fw/flood-$mcu.elf" || exit 1
done
export R2S_E2E_AVR_FW=$build/fw
[ "$build_only" = 1 ] && exit 0

# The painted peaks the scenarios record, one entry per firmware image,
# merged to the max across scenarios and RMW legs. Reset every run, so a
# stale peak from an older firmware can never leak into the table.
export R2S_E2E_AVR_STACK_OUT=$build/measured-stack.json
echo '{}' >"$R2S_E2E_AVR_STACK_OUT" || exit 1

# The static figures beside the peaks need .su files, which avr-gcc does
# not write under the shipped link-time optimized flags, so each e2e
# firmware is rebuilt without LTO under stack-fw/, the same pattern
# scripts/build-examples.sh uses for its stack/ dir. Same sources and
# flags, only the optimizer differs, and the table names which binary
# each column comes from.
for flavor in slice flood; do
  sketch=avr_slice
  [ "$flavor" = flood ] && sketch=avr_flood
  for board in uno mega; do
    mcu=atmega328p
    [ "$board" = mega ] && mcu=atmega2560
    label="$flavor-$mcu"
    sw="$build/stack-fw/$label"
    rm -rf "$sw"
    mkdir -p "$sw" || exit 1
    cppflags="$serial_flags -fno-lto -fstack-usage"
    [ "$flavor" = flood ] && cppflags="$cppflags -DR2S_TEST_TX_CAP=16"
    arduino-cli compile --fqbn "arduino:avr:$board" \
      --build-property "compiler.c.extra_flags=$extras -fno-lto -fstack-usage" \
      --build-property "compiler.cpp.extra_flags=$cppflags" \
      --build-property "compiler.c.elf.extra_flags=-fno-lto" \
      --library "$stage" --build-path "$sw" "tests/e2e/$sketch" || exit 1
    mv "$sw/$sketch.ino.elf" "$sw/$label.elf" || exit 1
  done
done
PYTHONPATH="$root/tools" python3 - "$build/stack-fw" "$build/static-stack.json" <<'EOF' || exit 1
import json
import sys
from pathlib import Path

import stack_report

root, out = Path(sys.argv[1]), Path(sys.argv[2])
statics = {}
for entry in sorted(root.iterdir()):
    if not entry.is_dir():
        continue
    elfs = sorted(entry.glob("*.elf"))
    if len(elfs) != 1:
        print(f"run-avr: {entry} holds no single firmware ELF", file=sys.stderr)
        raise SystemExit(1)
    statics[entry.name] = stack_report.static_worst(str(elfs[0]), entry)["total"]
out.write_text(json.dumps(statics, indent=2) + "\n", encoding="utf-8")
EOF

command -v colcon >/dev/null 2>&1 || { echo "colcon not found: run it in a ROS container"; exit 1; }
: "${ROS_DISTRO:?ROS_DISTRO is not set: run it in a ROS container}"
if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/$ROS_DISTRO/setup.bash" ]; then
  # shellcheck source=/dev/null
  set +u
  source "/opt/ros/$ROS_DISTRO/setup.bash"
  set -u
fi
export R2S_BUILD_DIR=$build

bash scripts/bridge-test.sh --build-only || exit 1
# shellcheck source=/dev/null
set +u
source "$build/bridge/install/setup.bash"
set -u

# The scenario files import the framework as a package; direct launch_test
# runs need the same directory on the import path.
export PYTHONPATH=$root/tests/e2e${PYTHONPATH:+:$PYTHONPATH}
# The helper assertions run before any scenario, so a broken helper fails
# fast instead of mid-handshake; the script directory is already on the
# import path when Python runs the file directly.
python3 tests/e2e/checks_test.py || exit 1
python3 tests/e2e/test_avr_support.py || exit 1

# One DDS domain per suite run, so parallel runs on the shared host never see
# each other's topics: every process below inherits the same domain.
export ROS_DOMAIN_ID=${R2S_E2E_DOMAIN_ID:?e2e-lease.sh provides it}
echo "== e2e isolation: ROS_DOMAIN_ID=$ROS_DOMAIN_ID ZENOH_ROUTER_PORT=$R2S_ZENOH_ROUTER_PORT"

boards=${R2S_E2E_BOARDS:-"uno mega"}
rmws=${R2S_RMWS:-rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_zenoh_cpp}

status=0
failures=""
matrix_done=0
for rmw in $rmws; do
  echo "== e2e-avr: $rmw"
  export RMW_IMPLEMENTATION=$rmw
  router=""
  if [ "$rmw" = rmw_zenoh_cpp ]; then
    router=$(find "/opt/ros/$ROS_DISTRO" -name rmw_zenohd -type f 2>/dev/null | head -n 1)
    [ -n "$router" ] || { echo "rmw_zenohd not found for $ROS_DISTRO"; status=1; break; }
    # A router per run on the leased port, and every node pointed at it, so
    # concurrent suites never share a Zenoh session the way the DDS domain
    # separates the other legs. Multicast scouting stays off, as in the
    # distro default: otherwise every leased router joins one multicast
    # group and the suites merge no matter the port.
    router_config=$build/zenoh-router-$R2S_ZENOH_ROUTER_PORT.json5
    session_config=$build/zenoh-session-$R2S_ZENOH_ROUTER_PORT.json5
    printf '{\n  mode: "router",\n  scouting: { multicast: { enabled: false } },\n  listen: {\n    endpoints: ["tcp/127.0.0.1:%s"],\n  },\n}\n' "$R2S_ZENOH_ROUTER_PORT" >"$router_config"
    printf '{\n  scouting: { multicast: { enabled: false } },\n  connect: {\n    endpoints: ["tcp/127.0.0.1:%s"],\n  },\n}\n' "$R2S_ZENOH_ROUTER_PORT" >"$session_config"
    export ZENOH_ROUTER_CONFIG_URI=$router_config
    export ZENOH_SESSION_CONFIG_URI=$session_config
    "$router" >/dev/null 2>&1 &
    router=$!
    sleep 2
    kill -0 "$router" 2>/dev/null || { echo "rmw_zenohd on port $R2S_ZENOH_ROUTER_PORT died"; status=1; break; }
  fi
  for board in $boards; do
    export R2S_E2E_BOARD=$board
    for scenario in 1 2 3 4 12; do
      # Same R2S_E2E_SKIP contract as run.sh, matched on the avr file name.
      case " ${R2S_E2E_SKIP:-} " in
        *" test_scenario_${scenario}_avr "*) echo "skip test_scenario_${scenario}_avr (R2S_E2E_SKIP)" ;;
        *)
          timeout 300 launch_test "tests/e2e/test_scenario_${scenario}_avr.py"
          code=$?
          # One line per failed scenario file, so a nonzero exit always
          # names its cause even when the failure scrolled out of the tail.
          if [ "$code" -ne 0 ]; then
            status=1
            line="e2e-failure: test_scenario_${scenario}_avr board=$board rmw=$rmw exit=$code"
            [ "$code" -eq 124 ] && line="$line (timeout)"
            echo "$line"
            failures="$failures$line
"
          fi
          ;;
      esac
    done
  done
  # The baud matrix runs once, under the first RMW of the loop: one
  # representative flow per baud per board keeps the e2e-avr job small.
  if [ "$matrix_done" = 0 ]; then
    matrix_done=1
    for board in $boards; do
      export R2S_E2E_BOARD=$board
      for baud in $bauds; do
        export R2S_E2E_BAUD=$baud
        case " ${R2S_E2E_SKIP:-} " in
          *" test_baud_matrix_avr "*) echo "skip test_baud_matrix_avr (R2S_E2E_SKIP)" ;;
          *)
            timeout 300 launch_test "tests/e2e/test_baud_matrix_avr.py"
            code=$?
            # Same per-failure line as the scenario loop, plus the baud:
            # one matrix row per baud per board shares the file name.
            if [ "$code" -ne 0 ]; then
              status=1
              line="e2e-failure: test_baud_matrix_avr board=$board baud=$baud rmw=$rmw exit=$code"
              [ "$code" -eq 124 ] && line="$line (timeout)"
              echo "$line"
              failures="$failures$line
"
            fi
            ;;
        esac
      done
    done
  fi
  if [ -n "$router" ]; then kill "$router" 2>/dev/null || true; wait "$router" 2>/dev/null || true; fi
done
# A final list of every failure, so the report names each one whatever
# the output length.
if [ -n "$failures" ]; then
  echo "== e2e failures:"
  printf '%s' "$failures"
fi

# The report-only stack table: painted peaks next to the static figures.
# Never judged, so firmware without a measurement (a skipped scenario)
# warns here and reads n/a instead of failing the run.
PYTHONPATH="$root/tools" python3 - "$build/static-stack.json" "$R2S_E2E_AVR_STACK_OUT" \
  "$build/stack-paint-table.md" <<'EOF'
import json
import sys
from pathlib import Path

import stack_report

statics = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
try:
    measured = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
except (OSError, ValueError):
    measured = {}
if not isinstance(measured, dict):
    measured = {}
for name in sorted(statics):
    if name not in measured:
        print(f"run-avr: no measured stack for {name}", file=sys.stderr)
Path(sys.argv[3]).write_text(stack_report.measured_table(statics, measured), encoding="utf-8")
EOF
table_status=$?
if [ "$table_status" -ne 0 ]; then
  echo "run-avr: the stack table failed" >&2
  status=1
else
  echo "== stack paint vs static"
  cat "$build/stack-paint-table.md"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
      echo "### Stack paint vs static"
      echo
      cat "$build/stack-paint-table.md"
    } >>"$GITHUB_STEP_SUMMARY"
  fi
fi
exit $status
