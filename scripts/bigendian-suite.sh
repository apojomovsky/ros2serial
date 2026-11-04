#!/usr/bin/env bash
# The big-endian test scope: runs inside the docker/bigendian image, never in
# a pull request check. It cross-builds the core unit and vector tests for
# s390x and runs them under qemu-s390x, then runs the generator reference
# suite and the vector check with the image's Python. The bridge stays out:
# rclcpp is not available cross-compiled, so nothing here imports ROS, and
# the ROS-dependent generator tests skip without an underlay. The generator
# model tests load a native host library: they prove the reference codec,
# while the qemu run proves the core. Usage, from the repository root inside
# the image: bash scripts/bigendian-suite.sh
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
need() {
  command -v "$1" >/dev/null 2>&1 && return 0
  echo "bigendian-suite.sh: $1 not found: run inside the docker/bigendian image" >&2
  return 1
}
need s390x-linux-gnu-gcc && need s390x-linux-gnu-g++ && need qemu-s390x \
  && need cmake && need ninja && need python3 || exit 2
python3 -c "import pytest" 2>/dev/null || {
  echo "bigendian-suite.sh: pytest not found: run inside the docker/bigendian image" >&2
  exit 2
}

# A scratch tree under /tmp: the mounted repository stays clean, so the job
# leaves no build output behind it.
build=${R2S_BIGENDIAN_BUILD:-/tmp/r2s-bigendian}
rm -rf "$build"
mkdir -p "$build/core" "$build/scratch" || exit 1

# The core unit and vector tests, built for s390x as the gcc config builds
# them for the host, and run under the emulator: CMAKE_CROSSCOMPILING_EMULATOR
# prefixes every ctest entry with qemu-s390x and its sysroot.
cmake -S core -B "$build/core" -G Ninja \
  -DCMAKE_C_COMPILER=s390x-linux-gnu-gcc -DCMAKE_CXX_COMPILER=s390x-linux-gnu-g++ \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  "-DCMAKE_CROSSCOMPILING_EMULATOR=qemu-s390x;-L;/usr/s390x-linux-gnu" \
  >"$build/core-configure.log" 2>&1 || { cat "$build/core-configure.log"; exit 1; }
cmake --build "$build/core" >"$build/core-build.log" 2>&1 || { tail -20 "$build/core-build.log"; exit 1; }
ctest --test-dir "$build/core" --output-on-failure --schedule-random || exit 1

# The generator reference suite with the image's Python, from the scratch
# directory: hypothesis writes its example database under the working
# directory, which must not be the mounted repository.
host_dir="$build/host"
bash scripts/build-host-lib.sh "$host_dir" >"$build/host-build.log" 2>&1 || { tail -20 "$build/host-build.log"; exit 1; }
export R2S_HOST_LIB_DIR="$host_dir"
export PYTHONPATH="$root/gen"
export PYTHONDONTWRITEBYTECODE=1
(cd "$build/scratch" && python3 -m pytest "$root/gen" -p no:cacheprovider) || exit 1

# Every vector file still validates: the C runners above replay them.
python3 tools/vectors_check.py || exit 1
echo "big-endian: core under qemu-s390x, generator reference and vectors passed"
