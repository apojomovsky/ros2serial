"""Worst-case stack budgets: static analysis from .su files and the call graph.

avr-gcc -fstack-usage writes one .su file per compiled unit; avr-objdump -d
gives the call edges from rcall targets. The core allows no recursion and no
indirect calls (the zone check enforces both), so the deepest path from an
entry is the static worst case. Functions without .su data (precompiled
Arduino core, libc) count as zero and are listed as unknown; dynamic frames
are listed, not summed. Measured stack painting in simavr (P7) will stand
next to this figure.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

OBJDUMP = os.environ.get("R2S_AVR_OBJDUMP", "avr-objdump")
CXXFILT = os.environ.get("R2S_AVR_CXXFILT", "avr-c++filt")

LABEL = re.compile(r"^([0-9a-fA-F]+)\s+<([^>]+)>:")
INSN = re.compile(r"^\s*([0-9a-fA-F]+):")
CALL = re.compile(r"\s(?:call|rcall)\s+[^\n;]*;\s*0x([0-9a-fA-F]+)(?:\s*<([^>]+)>)?")
OFFSET = re.compile(r"\+0x[0-9a-fA-F]+$")
LOCATION = re.compile(r"^(.*?):([0-9]+):([0-9]+):(.+)$")


def _canon(name: str) -> str:
    """The link name: C++ signatures and return types stripped.

    avr-gcc records C++ frames with their full signature, while objdump
    labels the same function demangled; both reduce to the qualified name.
    """
    return name.split("(")[0].rsplit(" ", 1)[-1]


class StackError(Exception):
    """The stack inputs cannot be used as given."""


def parse_su(text: str) -> dict:
    """Map function names to (frame bytes, static or dynamic) from .su text.

    Two generations of the format exist: the old name:size:kind lines and
    the newer location lines, where tabs separate file:line:col:name from the
    size and the kind.
    """
    sizes = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        fields = line.split("\t")
        if len(fields) == 3:
            located = LOCATION.match(fields[0])
            if located is None:
                raise StackError(f"not a .su line: {line[:120]}")
            name = _canon(located.group(4))
            number, kind = fields[1], fields[2]
        elif len(line.split(":")) == 3:
            name, number, kind = line.split(":")
            name = _canon(name)
        else:
            raise StackError(f"not a .su line: {line[:120]}")
        if not number.isdigit():
            raise StackError(f"not a .su line: {line[:120]}")
        if kind == "static":
            sizes[name] = (int(number), "static")
        elif kind.startswith("dynamic"):
            sizes[name] = (int(number), "dynamic")
        else:
            raise StackError(f"unknown .su frame kind in: {line[:120]}")
    return sizes


def _demangle(names: set) -> dict:
    """Demangle Itanium names in one c++filt run; unknown lines pass through."""
    mangled = sorted(n for n in names if n.startswith("_Z"))
    if not mangled:
        return {}
    try:
        proc = subprocess.run([CXXFILT], input="\n".join(mangled), capture_output=True, text=True, check=False)
    except OSError as error:
        raise StackError(f"{CXXFILT} cannot run: {error}") from error
    if proc.returncode != 0:
        raise StackError(f"{CXXFILT} failed: {proc.stderr.strip()[:200]}")
    demangled = proc.stdout.splitlines()
    pairs = zip(mangled, demangled, strict=False)
    if len(demangled) != len(mangled):
        raise StackError(f"{CXXFILT} answered {len(demangled)} lines for {len(mangled)} names")
    return dict(pairs)


def parse_calls(text: str) -> dict:
    """Map callers to callees from avr-objdump -d output."""
    addresses = {}
    for line in text.splitlines():
        match = LABEL.match(line)
        if match:
            addresses[match.group(1).lower().lstrip("0") or "0"] = match.group(2)
    demangled = _demangle(set(addresses.values()))

    def resolve(raw: str) -> str:
        return _canon(demangled.get(raw, raw))

    # Function entry addresses, by resolved name: a call into the middle of
    # its own caller is folded identical code the linker shared, not
    # recursion, since real recursion enters at the start address.
    starts = {}
    for address, raw in addresses.items():
        starts.setdefault(resolve(raw), address)

    calls: dict[str, list[str]] = {}
    caller = None
    at = 0
    for line in text.splitlines():
        label = LABEL.match(line)
        if label:
            caller = resolve(label.group(2))
            continue
        insn = INSN.match(line)
        if insn is not None:
            at = int(insn.group(1), 16)
        if caller is None:
            continue
        # A call whose target is the next instruction is a dead call the
        # linker relaxed away, unless a function starts there, in which case
        # the callee genuinely follows its caller.
        match = CALL.search(line)
        if not match:
            continue
        target, annotated = match.group(1), match.group(2)
        address = target.lower().lstrip("0") or "0"
        callee = addresses.get(address)
        if callee is None and annotated is not None:
            callee = OFFSET.sub("", annotated)
        if callee is not None:
            callee = resolve(callee)
            instruction = 4 if "\tcall\t" in line else 2
            if int(address, 16) == at + instruction and address != starts.get(callee, None):
                continue
            if callee == caller and address != starts.get(callee, address):
                continue
            if callee not in calls.setdefault(caller, []):
                calls[caller].append(callee)
    return calls


def worst_stack(sizes: dict, calls: dict, entries: list) -> dict:
    """The deepest static path from the entries, with gaps listed."""
    best: dict = {"total": 0, "path": [], "unknown": [], "dynamic": [], "cycles": []}
    unknown: set[str] = set()
    dynamic: set[str] = set()

    def visit(name: str, stack: list) -> tuple[int, list]:
        if name in stack:
            best["cycles"].append(stack[stack.index(name) :] + [name])
            return 0, []
        size, kind = sizes.get(name, (0, "static"))
        if name not in sizes:
            unknown.add(name)
        elif kind == "dynamic":
            dynamic.add(name)
            size = 0
        depth, path = size, [name]
        for callee in calls.get(name, []):
            child_depth, child_path = visit(callee, stack + [name])
            if size + child_depth > depth:
                depth, path = size + child_depth, [name] + child_path
        return depth, path

    for entry in entries:
        depth, path = visit(entry, [])
        if depth > best["total"]:
            best["total"], best["path"] = depth, path
    best["unknown"] = sorted(unknown)
    best["dynamic"] = sorted(dynamic)
    return best


def _run(tool: str, *args: str) -> str:
    try:
        proc = subprocess.run([tool, *args], capture_output=True, text=True, check=False)
    except OSError as error:
        raise StackError(f"{tool} cannot run: {error}") from error
    if proc.returncode != 0:
        raise StackError(f"{tool} {' '.join(args)} failed: {proc.stderr.strip()[:200]}")
    return proc.stdout


def _stack_source(build_dir: Path, key: str) -> Path:
    """The ELF whose frames the stack number is computed from.

    Arduino ships link-time optimized firmware, under which avr-gcc writes no
    .su files, so the budget builds each sketch again without LTO under
    stack/ and analyzes that build. The cmake firmware carries no LTO, so its
    shipped ELF is analyzed directly.
    """
    stacked = build_dir / "stack" / key
    if stacked.is_file():
        return stacked
    return build_dir / key


def collect(build_dir: Path) -> dict:
    """Worst static stack per firmware, keyed like the size measurements."""
    elfs = [
        p
        for p in sorted(build_dir.rglob("*.elf"))
        if "build" not in p.relative_to(build_dir).parts and "stack" not in p.relative_to(build_dir).parts
    ]
    elfs += sorted(p for p in build_dir.rglob("hello_cmake") if p.is_file() and os.access(p, os.X_OK))
    if not elfs:
        raise StackError(f"no firmware under {build_dir}")
    stacks = {}
    for elf in sorted(elfs):
        name = elf.relative_to(build_dir).as_posix()
        source = _stack_source(build_dir, name)
        if not source.is_file():
            raise StackError(f"no stack build for {name}")
        sizes: dict = {}
        for path in sorted(source.parent.rglob("*.su")):
            sizes.update(parse_su(path.read_text(encoding="utf-8")))
        if not sizes:
            raise StackError(f"no .su files beside {source}")
        stacks[name] = worst_stack(sizes, parse_calls(_run(OBJDUMP, "-d", str(source))), ["main"])
    return stacks
    return stacks


def check_stacks(stacks: dict, budgets: dict) -> list:
    """One line per breached stack budget or call cycle; unbudgeted is skipped."""
    problems = []
    for name, worst in stacks.items():
        limits = budgets.get("firmware", {}).get(name)
        if limits is None:
            continue
        for cycle in worst.get("cycles", []):
            problems.append(f"{name}: call cycle {' -> '.join(cycle)} makes the stack unbounded")
        if "stack" not in limits:
            continue
        if worst.get("total", 0) > limits["stack"]:
            problems.append(f"{name}: stack {worst['total']} exceeds budget {limits['stack']}")
    return problems


def markdown_table(stacks: dict, budgets: dict, base: dict | None = None) -> str:
    """A PR comment table: head worst case against base and budget."""
    rows = ["| firmware | stack | stack budget |", "| --- | --- | --- |"]
    for name in sorted(stacks):
        worst = stacks[name]
        cell = str(worst.get("total", "?"))
        if base is not None and name in base:
            cell += f" (base {base[name].get('total', '?')})"
        if worst.get("unknown"):
            cell += f"; unknown: {', '.join(worst['unknown'][:3])}"
            if len(worst["unknown"]) > 3:
                cell += ", ..."
        budget = budgets.get("firmware", {}).get(name, {}).get("stack", "n/a")
        rows.append(f"| {name} | {cell} | {budget} |")
    return "\n".join(rows) + "\n"


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    collect_cmd = sub.add_parser("collect", help="collect worst stacks under a build directory")
    collect_cmd.add_argument("--build-dir", required=True)
    collect_cmd.add_argument("--out", required=True)
    check_cmd = sub.add_parser("check", help="judge stacks against budgets")
    check_cmd.add_argument("--stack", required=True)
    check_cmd.add_argument("--budgets", required=True)
    check_cmd.add_argument("--base")
    check_cmd.add_argument("--markdown")
    args = parser.parse_args(argv)
    if args.command == "collect":
        try:
            data = collect(Path(args.build_dir))
        except (StackError, OSError) as error:
            print(f"stack_report: error: {error}", file=sys.stderr)
            return 1
        Path(args.out).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        return 0
    try:
        from size_report import load_budgets

        stacks = json.loads(Path(args.stack).read_text(encoding="utf-8"))
        budgets = load_budgets(Path(args.budgets))
        problems = check_stacks(stacks, budgets)
    except (OSError, ValueError, StackError) as error:
        print(f"stack_report: error: {error}", file=sys.stderr)
        return 1
    if args.markdown:
        base = None
        if args.base:
            try:
                base = json.loads(Path(args.base).read_text(encoding="utf-8"))
            except (OSError, ValueError) as error:
                print(f"stack_report: error: {error}", file=sys.stderr)
                return 1
        Path(args.markdown).write_text(markdown_table(stacks, budgets, base), encoding="utf-8")
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
