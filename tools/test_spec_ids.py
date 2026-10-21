"""spec/protocol.md: every normative statement carries a well-formed, unique ID."""

import re
from collections import Counter
from pathlib import Path

SPEC = Path(__file__).resolve().parent.parent / "spec" / "protocol.md"
DEFINITION = re.compile(r"^\s*- \[([A-Z]{2,4}-\d+)\] ", re.MULTILINE)
ANY_ID = re.compile(r"\[([A-Z]{2,4}-\d+)\]")


def test_the_spec_defines_ids():
    assert DEFINITION.findall(SPEC.read_text())


def test_ids_are_unique():
    counts = Counter(DEFINITION.findall(SPEC.read_text()))
    assert [i for i, n in counts.items() if n > 1] == []


def test_every_referenced_id_is_defined():
    text = SPEC.read_text()
    defined = set(DEFINITION.findall(text))
    assert sorted(set(ANY_ID.findall(text)) - defined) == []


def test_ids_are_numbered_without_gaps_per_prefix():
    ids = DEFINITION.findall(SPEC.read_text())
    by_prefix: dict[str, list[int]] = {}
    for i in ids:
        prefix, n = i.split("-")
        by_prefix.setdefault(prefix, []).append(int(n))
    for prefix, numbers in by_prefix.items():
        assert numbers == list(range(1, len(numbers) + 1)), (prefix, numbers)
