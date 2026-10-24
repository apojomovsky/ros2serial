#!/usr/bin/env python3
"""Check the portable core's rules on what the compiler produced, not on the source.

Every C file under core/src, and every header under core/include through a stub
that keeps its inline functions, is compiled for the ATmega328P and inspected:
no indirect calls and no function addresses taken (function pointers), no calls
into the float routines of libgcc or avr-libc's libm (float arithmetic), and no
header from cpp/ among its dependencies. Jump tables are disabled for this build
only, because a switch table stores label addresses with the same relocations a
function pointer uses.

Because the receive ring is compiled only with its switch, every source and
header is checked with `R2S_WITH_RX_RING` defined, so the ring is inspected
instead of compiled away.

Two limits come with checking objects: only the ATmega328P configuration is
compiled, so code behind other #if branches is not seen, and float expressions
the compiler folds into constants never reach the object.

Exits 1 with one line per problem.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from functools import cache
from pathlib import Path

CC = os.environ.get("R2S_ZONE_CC", "avr-gcc")
OBJDUMP = os.environ.get("R2S_ZONE_OBJDUMP", "avr-objdump")
NM = os.environ.get("R2S_ZONE_NM", "avr-nm")
MCU = ["-mmcu=atmega328p"]
FLAGS = ["-std=c99", "-Os", *MCU, "-fno-jump-tables", "-ffunction-sections", "-DR2S_WITH_RX_RING"]
HEADER_FLAGS = ["-fkeep-inline-functions", "-fkeep-static-functions"]
INSTRUCTION = re.compile(r"^\s+[0-9a-f]+:\t(?:[0-9a-f]{2} )+\s*\t(\S+)", re.MULTILINE)
INDIRECT = {"icall", "eicall", "ijmp", "eijmp"}
ADDRESS_TAKEN = re.compile(r"\bR_AVR_\w*_(?:PM|GS)\w*\s+(\S+)")


def _run(args: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(args, capture_output=True, text=True, cwd=cwd)


def _first_error(stderr: str) -> str:
    lines = stderr.strip().splitlines()
    return next((ln for ln in lines if "error:" in ln), lines[0] if lines else "(no output)")


@cache
def float_symbols() -> frozenset[str]:
    """Every symbol avr-libc's libm defines, plus libgcc's single-float helpers."""
    names: set[str] = set()
    for flag in ("-print-file-name=libm.a", "-print-libgcc-file-name"):
        lib = _run([CC, *MCU, flag]).stdout.strip()
        if not lib or not Path(lib).is_file():
            continue
        out = _run([NM, "--defined-only", "-g", lib]).stdout
        syms = {line.split()[-1] for line in out.splitlines() if re.match(r"^[0-9a-f]+ [TtWw] ", line)}
        names |= syms if flag.endswith("libm.a") else {s for s in syms if "sf" in s}
    return frozenset(names)


def _deps(text: str) -> list[str]:
    # Make syntax: line continuations, and spaces inside a path escaped as "\ ".
    body = text.replace("\\\n", " ").split(":", 1)[-1]
    return [p.replace("\\ ", " ") for p in re.split(r"(?<!\\)\s+", body.strip()) if p]


def inspect(root: Path, name: str, unit: Path, obj: Path, extra: list[str]) -> list[str]:
    include = ["-I", str(root / "core/include")]
    problems = []

    deps = _run([CC, *FLAGS, *include, "-MM", "-MG", str(unit)], cwd=root)
    if deps.returncode != 0:
        problems.append(f"{name}: dependency scan failed: {_first_error(deps.stderr)}")
    cpp_dir = (root / "cpp").resolve()
    for dep in _deps(deps.stdout):
        path = Path(dep)
        if not path.is_absolute():
            # Found headers are relative to the compiler's cwd (the root); missing
            # ones (-MG) keep their spelling, which is relative to the including file.
            path = root / dep if (root / dep).exists() else unit.parent / dep
        resolved = path.resolve()
        if cpp_dir in resolved.parents:
            header = resolved.relative_to(root.resolve())
            problems.append(f"{name}: includes {header}; the core may not use the C++ layer")

    built = _run([CC, *FLAGS, *extra, *include, "-c", str(unit), "-o", str(obj)], cwd=root)
    if built.returncode != 0:
        return problems + [f"{name}: does not compile for AVR: {_first_error(built.stderr)}"]

    disasm = _run([OBJDUMP, "-dr", str(obj)])
    relocs = _run([OBJDUMP, "-r", str(obj)])
    undefined = _run([NM, "-u", str(obj)])
    for tool, result in ((OBJDUMP, disasm), (OBJDUMP, relocs), (NM, undefined)):
        if result.returncode != 0:
            problems.append(f"{name}: {tool} failed: {_first_error(result.stderr)}")
    if any(m in INDIRECT for m in INSTRUCTION.findall(disasm.stdout)):
        problems.append(f"{name}: indirect call or jump (function pointer) in the compiled code")
    for m in ADDRESS_TAKEN.finditer(disasm.stdout + relocs.stdout):
        target = m.group(1).removeprefix(".text.").split("+")[0]
        problems.append(f"{name}: takes the address of {target} (function pointer)")
    forbidden = float_symbols()
    for sym in undefined.stdout.split():
        if sym in forbidden or re.match(r"^__\w*sf\w*$", sym):
            problems.append(f"{name}: calls the float routine {sym} (float arithmetic)")
    return list(dict.fromkeys(problems))


def check(root: Path) -> list[str]:
    for tool in (CC, OBJDUMP, NM):
        if shutil.which(tool) is None:
            return [
                f"zone check: {tool} not found (run it in the dev image: make exec CMD='python3 tools/zone_check.py')"
            ]
    sources = sorted((root / "core/src").rglob("*.c")) if (root / "core/src").is_dir() else []
    headers = sorted((root / "core/include").rglob("*.h")) if (root / "core/include").is_dir() else []
    problems: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for src in sources:
            rel = src.relative_to(root).as_posix()
            problems += inspect(root, rel, src, work / (rel.replace("/", "__") + ".o"), [])
        for header in headers:
            rel = header.relative_to(root).as_posix()
            stub = work / (rel.replace("/", "__") + ".c")
            stub.write_text(f'#include "{header}"\n')
            problems += inspect(root, rel, stub, stub.with_suffix(".o"), HEADER_FLAGS)
    return problems


def main() -> int:
    problems = check(Path(__file__).resolve().parent.parent)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
