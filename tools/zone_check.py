#!/usr/bin/env python3
"""Check the portable core's rules on what the compiler produced, not on the source.

Each core/src/*.c is compiled for the ATmega328P and inspected: no indirect
calls and no function addresses taken (function pointers), no calls into the
soft-float library (float arithmetic), and no header from cpp/ among its
dependencies. Jump tables are disabled for this build only, because a switch
table stores label addresses with the same relocations a function pointer uses.
Exits 1 with one line per problem.
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CC = os.environ.get("R2S_ZONE_CC", "avr-gcc")
OBJDUMP = os.environ.get("R2S_ZONE_OBJDUMP", "avr-objdump")
NM = os.environ.get("R2S_ZONE_NM", "avr-nm")
FLAGS = ["-std=c99", "-Os", "-mmcu=atmega328p", "-fno-jump-tables", "-ffunction-sections"]
INDIRECT = re.compile(r"\b(e?icall|e?ijmp)\b")
ADDRESS_TAKEN = re.compile(r"\bR_AVR_\w*_(PM|GS)\w*\s+(\S+)")
SOFT_FLOAT = re.compile(
    r"^__((add|sub|mul|div|neg)sf3|fix(uns)?sf\w+|float(un)?\w*sf|(cmp|eq|ne|lt|le|gt|ge|unord)sf2|fp_\w+)$"
)


def _run(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(args, capture_output=True, text=True)


def check_source(root: Path, src: Path, workdir: Path) -> list[str]:
    name = src.relative_to(root).as_posix()
    include = ["-I", str(root / "core/include")]
    obj = workdir / (src.stem + ".o")
    built = _run([CC, *FLAGS, *include, "-c", str(src), "-o", str(obj)])
    if built.returncode != 0:
        first = (built.stderr.strip().splitlines() or ["(no output)"])[0]
        return [f"{name}: does not compile for AVR: {first}"]
    problems = []

    deps = _run([CC, *FLAGS, *include, "-MM", str(src)]).stdout.replace("\\\n", " ").split()
    cpp_dir = (root / "cpp").resolve()
    for dep in deps[1:]:
        if cpp_dir in Path(dep).resolve().parents:
            header = Path(dep).resolve().relative_to(root.resolve())
            problems.append(f"{name}: includes {header}; the core may not use the C++ layer")

    disasm = _run([OBJDUMP, "-dr", str(obj)]).stdout
    if INDIRECT.search(disasm):
        problems.append(f"{name}: indirect call or jump (function pointer) in the compiled code")
    for m in ADDRESS_TAKEN.finditer(disasm + _run([OBJDUMP, "-r", str(obj)]).stdout):
        target = m.group(2).removeprefix(".text.").split("+")[0]
        problems.append(f"{name}: takes the address of {target} (function pointer)")

    for sym in _run([NM, "-u", str(obj)]).stdout.split():
        if SOFT_FLOAT.match(sym):
            problems.append(f"{name}: calls the soft-float routine {sym} (float arithmetic)")
    return list(dict.fromkeys(problems))


def check(root: Path) -> list[str]:
    sources = sorted((root / "core/src").glob("*.c"))
    if not sources:
        return []
    with tempfile.TemporaryDirectory() as workdir:
        problems = []
        for src in sources:
            problems += check_source(root, src, Path(workdir))
        return problems


def main() -> int:
    problems = check(Path(__file__).resolve().parent.parent)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
