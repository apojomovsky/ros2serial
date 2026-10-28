#!/usr/bin/env bash
# Build the release artifacts: the Arduino library zip from the G11 bundle and
# the Python sdist and wheel. The release workflow runs this in the dev image
# and attaches what it prints to the pre-release; a maintainer runs the same
# command locally for the dry run. Nothing here publishes anywhere.
# Usage: build-release-artifacts.sh [--distro <name>] [--out <dir>]
set -euo pipefail
usage() { echo "usage: build-release-artifacts.sh [--distro <name>] [--out <dir>]"; exit 2; }
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
distro=jazzy
out=dist
while [ $# -gt 0 ]; do
  case $1 in
    --distro) [ $# -ge 2 ] || usage; distro=$2; shift 2 ;;
    --out) [ $# -ge 2 ] || usage; out=$2; shift 2 ;;
    *) usage ;;
  esac
done
version=$(tr -d '[:space:]' <"$root/VERSION")
printf '%s' "$version" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' || {
  echo "build-release-artifacts: VERSION '$version' is not major.minor.patch" >&2
  exit 1
}
rm -rf "$out"
mkdir -p "$out"
case $out in /*) ;; *) out=$PWD/$out ;; esac
PYTHONPATH="$root/gen" python3 - "$root" "$distro" "$out" <<'EOF'
import sys
from pathlib import Path

root, distro, out = (sys.argv[1], sys.argv[2], sys.argv[3])
sys.path.insert(0, str(Path(root) / "gen"))
from r2s_gen import bundle

files = bundle.bundle(distro)
staged = Path(out) / "ros2serial"
(staged / "src").mkdir(parents=True)
for name, text in files.items():
    (staged / name).write_text(text, encoding="utf-8")
EOF
# The bundle carries only the descriptors and the sketch header: the core
# sources join it here, the same staging the example build uses, so the
# library compiles on its own.
mkdir -p "$out/ros2serial/src/r2s"
cp "$root"/core/include/r2s/*.h "$out/ros2serial/src/r2s/"
cp "$root"/core/src/*.c "$out/ros2serial/src/"
rm -f "$out/ros2serial/src/test_reset.c"
python3 - "$out" "$version" <<'EOF'
import sys
import zipfile
from pathlib import Path

out, version = (sys.argv[1], sys.argv[2])
zip_path = Path(out) / f"ros2serial-arduino-{version}.zip"
names = sorted(str(path.relative_to(out)) for path in sorted((Path(out) / "ros2serial").rglob("*")) if path.is_file())
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
    for name in names:
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.external_attr = 0o644 << 16
        archive.writestr(info, (Path(out) / name).read_bytes())
print(zip_path.name)
EOF
(cd "$root/gen" && python3 setup.py -q sdist --dist-dir "$out" bdist_wheel --dist-dir "$out")
rm -rf "$out/ros2serial"
ls "$out"
