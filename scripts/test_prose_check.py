import prose_check

EM = "\u2014"


def diff(path: str, added: list[str], start: int = 1) -> str:
    lines = [f"diff --git a/{path} b/{path}", f"+++ b/{path}", f"@@ -0,0 +{start},{len(added)} @@"]
    return "\n".join(lines + [f"+{a}" for a in added]) + "\n"


def blocks(text: str):
    return prose_check.added_comment_blocks(text)


def test_python_and_c_comment_blocks_are_found():
    found = blocks(
        diff("tools/a.py", ["# why one", "# why two", "x = 1"])
        + diff("core/src/crc.c", ["/* reflected, so the residue is 0", " * when appended LE */", "int x;"], start=10)
    )
    assert [(b.path, b.line, len(b.lines)) for b in found] == [("tools/a.py", 1, 2), ("core/src/crc.c", 10, 2)]


def test_shebangs_and_code_are_not_comments():
    assert blocks(diff("scripts/x.sh", ["#!/usr/bin/env bash", "echo hi"])) == []


def test_nine_line_block_fails_eight_passes():
    eight = diff("a.py", [f"# line {i}" for i in range(8)])
    nine = diff("a.py", [f"# line {i}" for i in range(9)])
    assert prose_check.verify(eight) == []
    assert any("9 lines" in p for p in prose_check.verify(nine))


def test_decoration_is_rejected():
    assert any("decoration" in p for p in prose_check.verify(diff("a.c", ["/* @brief does things */"])))
    assert any("decoration" in p for p in prose_check.verify(diff("a.py", ["# ----------------"])))


def test_narrative_is_rejected():
    assert any("narrative" in p for p in prose_check.verify(diff("a.py", ["# fixed by moving the reset"])))
    assert any("narrative" in p for p in prose_check.verify(diff("a.py", ["# this PR changes the order"])))


def test_em_dash_in_comments_and_markdown_is_rejected():
    assert any("em-dash" in p for p in prose_check.verify(diff("a.py", [f"# one {EM} two"])))
    assert any("em-dash" in p for p in prose_check.verify(diff("docs/x.md", [f"Prose {EM} more"])))


def test_todo_needs_a_reason():
    assert any("TODO" in p for p in prose_check.verify(diff("a.py", ["# TODO"])))
    assert prose_check.verify(diff("a.py", ["# TODO: drop once C1 lands and the real framer exists"])) == []


def test_a_clean_comment_passes():
    assert prose_check.verify(diff("a.py", ["# CRC-16/MCRF4XX: reflected, so a little-endian append leaves 0."])) == []


def test_vendored_code_keeps_its_own_style():
    long_block = [f"/* line {i} */" for i in range(12)] + ["/* @brief upstream doc */"]
    assert prose_check.verify(diff("core/tests/vendor/unity/unity.h", long_block)) == []
    assert blocks(diff("core/tests/vendor/unity/README.md", [f"x {EM} y"])) == []
