"""spec/protocol.md: every normative statement carries a well-formed, unique ID."""

import re
import sys
from collections import Counter
from pathlib import Path

import pytest

SPEC = Path(__file__).resolve().parent.parent / "spec" / "protocol.md"
DEFINITION = re.compile(r"^\s*- \[([A-Z]{2,4}-\d+)\] ", re.MULTILINE)
ANY_ID = re.compile(r"\[([A-Z]{2,4}-\d+)\]")
# A bare mention such as "(ENC-10)" or "see VAR-2" counts as a reference too.
BARE_ID = re.compile(r"(?<![\w-])([A-Z]{2,4})-(\d+)\b")


def _spec_text() -> str:
    try:
        return SPEC.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as e:
        pytest.fail(f"spec/protocol.md: unreadable ({str(e).splitlines()[0]})")


def test_the_spec_defines_ids():
    assert DEFINITION.findall(_spec_text())


def test_ids_are_unique():
    counts = Counter(DEFINITION.findall(_spec_text()))
    assert [i for i, n in counts.items() if n > 1] == []


def test_every_referenced_id_is_defined():
    text = _spec_text()
    defined = set(DEFINITION.findall(text))
    assert sorted(set(ANY_ID.findall(text)) - defined) == []


def test_every_bare_mention_of_a_known_prefix_is_defined():
    text = _spec_text()
    defined = set(DEFINITION.findall(text))
    prefixes = {i.split("-")[0] for i in defined}
    mentioned = {f"{p}-{n}" for p, n in BARE_ID.findall(text) if p in prefixes}
    assert sorted(mentioned - defined) == []


def test_ids_are_numbered_without_gaps_per_prefix():
    ids = DEFINITION.findall(_spec_text())
    by_prefix: dict[str, list[int]] = {}
    for i in ids:
        prefix, n = i.split("-")
        by_prefix.setdefault(prefix, []).append(int(n))
    for prefix, numbers in by_prefix.items():
        assert numbers == list(range(1, len(numbers) + 1)), (prefix, numbers)


def test_a_spec_that_is_not_utf8_fails(tmp_path, monkeypatch):
    bad = tmp_path / "protocol.md"
    bad.write_bytes(b"- [AAA-1] One rule.\n\xff")
    monkeypatch.setattr(sys.modules[__name__], "SPEC", bad)
    with pytest.raises(pytest.fail.Exception, match="spec/protocol\\.md"):
        test_the_spec_defines_ids()
