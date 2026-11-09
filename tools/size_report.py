# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

"""Flash and RAM budgets: measure firmware ELFs and check them against budgets.yaml.

avr-size prints the Berkeley text/data/bss split. Flash holds the image
(the code plus the initialized data image) and static RAM holds data plus
bss. Xtensa firmware (the ESP32 builds) measures through the matching
xtensa-esp32-elf-size, whose Berkeley table reads the same way. Firmware
the budgets file does not name is reported, never judged, so a new example
cannot fail the gate before its budget is set.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import yaml

AVRSIZE = os.environ.get("R2S_AVR_SIZE", "avr-size")
AVRNM = os.environ.get("R2S_AVR_NM", "avr-nm")

# The Xtensa machine number: ESP32 firmware measures with the Xtensa tools,
# every other (or unreadable) ELF with the AVR ones.
EM_XTENSA = 0x5E


def _arduino_data_dir() -> Path:
    """Where the ESP32 core lives: the scratch directory the example build
    manages, defaulting under the shared cache like the build script."""
    cache = os.environ.get("R2S_CACHE_DIR", os.path.join(os.path.expanduser("~"), ".cache", "ros2serial"))
    return Path(os.environ.get("R2S_ARDUINO_DATA_DIR", os.path.join(cache, "arduino-esp32")))


def _xtensa_tool(filename: str, override_var: str) -> str:
    """The Xtensa binutil for the installed core: an explicit override wins,
    then the core's own toolchain, then the bare name, which fails loudly
    when no toolchain is installed."""
    override = os.environ.get(override_var)
    if override:
        return override
    matches = sorted(_arduino_data_dir().rglob(filename))
    if matches:
        return str(matches[0])
    return filename


def xtensa_size() -> str:
    """The size tool for ESP32 firmware (see _xtensa_tool)."""
    return _xtensa_tool("xtensa-esp32-elf-size", "R2S_XTENSA_SIZE")


def xtensa_nm() -> str:
    """The nm tool for ESP32 firmware (see _xtensa_tool)."""
    return _xtensa_tool("xtensa-esp32-elf-nm", "R2S_XTENSA_NM")


def elf_machine(path) -> int | None:
    """The ELF e_machine field of a firmware file, or None when unreadable.

    Reads only the 20 bytes the header needs, so a stubbed or truncated
    file reports None instead of failing the measurement.
    """
    try:
        with open(path, "rb") as handle:
            header = handle.read(20)
    except OSError:
        return None
    if len(header) < 20 or header[0:4] != b"\x7fELF":
        return None
    if header[5] == 1:
        order = "little"
    elif header[5] == 2:
        order = "big"
    else:
        return None
    return int.from_bytes(header[18:20], order)


class SizeError(Exception):
    """The measurement or the budgets file cannot be used as given."""


def parse_avr_size(text: str) -> dict:
    """Split an avr-size run into flash and static RAM byte counts."""
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 6 and all(part.isdigit() for part in fields[:3]):
            code, data, bss = (int(part) for part in fields[:3])
            return {"text": code, "data": data, "bss": bss, "flash": code + data, "ram": data + bss}
    raise SizeError(f"no avr-size table in: {text.strip()[:120]}")


def parse_nm_sizes(text: str) -> dict:
    """Map symbol names to sizes from avr-nm --print-size output."""
    sizes = {}
    for line in text.splitlines():
        match = re.match(r"^[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\S+\s+(\S+)", line)
        if match:
            sizes[match.group(2)] = int(match.group(1), 16)
    return sizes


def _run(tool: str, *args: str) -> str:
    try:
        proc = subprocess.run([tool, *args], capture_output=True, text=True, check=False)
    except OSError as error:
        raise SizeError(f"{tool} cannot run: {error}") from error
    if proc.returncode != 0:
        raise SizeError(f"{tool} {' '.join(args)} failed: {proc.stderr.strip()[:200]}")
    return proc.stdout


def measure(build_dir: Path) -> dict:
    """Measure every ELF under the build directory, keyed by relative path."""
    # Arduino keeps its intermediates under a build/ directory next to the
    # exported ELF, and the non-LTO stack builds live under stack/: neither
    # counts, so those copies are skipped.
    elfs = [
        p
        for p in sorted(build_dir.rglob("*.elf"))
        if "build" not in p.relative_to(build_dir).parts and "stack" not in p.relative_to(build_dir).parts
    ]
    elfs += sorted(p for p in build_dir.rglob("hello_cmake") if p.is_file() and os.access(p, os.X_OK))
    if not elfs:
        raise SizeError(f"no firmware under {build_dir}")
    measurements = {}
    for elf in sorted(elfs):
        name = elf.relative_to(build_dir).as_posix()
        if elf_machine(elf) == EM_XTENSA:
            size_tool, nm_tool = xtensa_size(), xtensa_nm()
        else:
            size_tool, nm_tool = AVRSIZE, AVRNM
        sizes = parse_avr_size(_run(size_tool, str(elf)))
        symbols = parse_nm_sizes(_run(nm_tool, "--print-size", str(elf)))
        if "r2s_runtime_topics" in symbols:
            sizes["topics_bytes"] = symbols["r2s_runtime_topics"]
        measurements[name] = sizes
    return measurements


def load_budgets(path: Path) -> dict:
    """Read the budgets file; anything but a firmware mapping is rejected."""
    try:
        budgets = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        raise SizeError(f"budgets file {path} cannot be read: {error}") from error
    if not isinstance(budgets, dict) or not isinstance(budgets.get("firmware"), dict):
        raise SizeError(f"budgets file {path} names no firmware mapping")
    return budgets


def check_sizes(measurements: dict, budgets: dict) -> list:
    """One line per breached budget; unbudgeted firmware is skipped."""
    problems = []
    for name, sizes in measurements.items():
        limits = budgets.get("firmware", {}).get(name)
        if limits is None:
            continue
        for metric in ("flash", "ram"):
            if metric not in limits:
                continue
            if metric not in sizes:
                raise SizeError(f"measurement for {name} names no {metric}")
            if sizes[metric] > limits[metric]:
                problems.append(f"{name}: {metric} {sizes[metric]} exceeds budget {limits[metric]}")
    return problems


def markdown_table(measurements: dict, budgets: dict, base: dict | None = None) -> str:
    """A PR comment table: head against base and budget per firmware."""
    rows = ["| firmware | flash | RAM | flash budget | RAM budget |", "| --- | --- | --- | --- | --- |"]
    for name in sorted(measurements):
        sizes = measurements[name]
        limits = budgets.get("firmware", {}).get(name, {})
        cells = [name, str(sizes.get("flash", "?")), str(sizes.get("ram", "?"))]
        if base is not None and name in base:
            cells[1] += f" (base {base[name].get('flash', '?')})"
            cells[2] += f" (base {base[name].get('ram', '?')})"
        cells += [str(limits.get("flash", "n/a")), str(limits.get("ram", "n/a"))]
        rows.append("| " + " | ".join(cells) + " |")
    return "\n".join(rows) + "\n"


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    measure_cmd = sub.add_parser("measure", help="measure ELFs under a build directory")
    measure_cmd.add_argument("--build-dir", required=True)
    measure_cmd.add_argument("--out", required=True)
    check_cmd = sub.add_parser("check", help="judge measurements against budgets")
    check_cmd.add_argument("--measurements", required=True)
    check_cmd.add_argument("--budgets", required=True)
    check_cmd.add_argument("--base")
    check_cmd.add_argument("--markdown")
    args = parser.parse_args(argv)
    if args.command == "measure":
        try:
            data = measure(Path(args.build_dir))
        except SizeError as error:
            print(f"size_report: error: {error}", file=sys.stderr)
            return 1
        Path(args.out).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        return 0
    try:
        measurements = json.loads(Path(args.measurements).read_text(encoding="utf-8"))
        budgets = load_budgets(Path(args.budgets))
        problems = check_sizes(measurements, budgets)
    except (OSError, ValueError, SizeError) as error:
        print(f"size_report: error: {error}", file=sys.stderr)
        return 1
    if args.markdown:
        base = None
        if args.base:
            try:
                base = json.loads(Path(args.base).read_text(encoding="utf-8"))
            except (OSError, ValueError) as error:
                print(f"size_report: error: {error}", file=sys.stderr)
                return 1
        Path(args.markdown).write_text(markdown_table(measurements, budgets, base), encoding="utf-8")
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
