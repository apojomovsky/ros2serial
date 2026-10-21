from pathlib import Path

import spec_coverage

SPEC = """# spec

- [AAA-1] One rule.
- [AAA-2] Another rule.
- [BBB-1] Retired.
"""


def tree(root: Path, files: dict[str, str]) -> Path:
    for name, text in {"spec/protocol.md": SPEC, **files}.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_text(text)
    return root


def problems(root: Path) -> list[str]:
    return spec_coverage.check(root).problems


def test_a_fully_covered_spec_passes(tmp_path):
    root = tree(
        tmp_path,
        {
            "spec/vectors/rcobs/a.yaml": "covers: [AAA-1]\ncases: []\n",
            "core/tests/test_a.c": "/* covers: AAA-2 */\nvoid test_a(void) {}\n",
        },
    )
    assert problems(root) == []


def test_an_uncovered_id_fails(tmp_path):
    root = tree(tmp_path, {"spec/vectors/rcobs/a.yaml": "covers: [AAA-1]\ncases: []\n"})
    assert [p for p in problems(root) if "AAA-2" in p and "no vector or test" in p]


def test_a_test_naming_an_unknown_id_fails_with_its_location(tmp_path):
    root = tree(
        tmp_path,
        {
            "spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2]\ncases: []\n",
            "gen/tests/test_x.py": "def test_x():\n    pass\n\n\n# covers: AAA-9\ndef test_y():\n    pass\n",
        },
    )
    assert problems(root) == ["gen/tests/test_x.py:5: names AAA-9, which spec/protocol.md does not define"]


def test_a_vector_naming_an_unknown_id_fails(tmp_path):
    root = tree(tmp_path, {"spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2, CCC-1]\ncases: []\n"})
    assert problems(root) == ["spec/vectors/rcobs/a.yaml: names CCC-1, which spec/protocol.md does not define"]


def test_a_duplicated_id_fails(tmp_path):
    root = tree(tmp_path, {"spec/protocol.md": SPEC + "- [AAA-1] Again.\n"})
    assert any("AAA-1 is defined 2 times" in p for p in problems(root))


def test_retired_ids_need_no_coverage_and_may_not_be_claimed(tmp_path):
    root = tree(tmp_path, {"spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2]\ncases: []\n"})
    assert problems(root) == []
    (root / "core/tests").mkdir(parents=True)
    (root / "core/tests/test_b.c").write_text("// covers: BBB-1\n")
    assert problems(root) == ["core/tests/test_b.c:1: names BBB-1, which is retired"]


def test_markers_take_every_common_comment_form(tmp_path):
    root = tree(
        tmp_path,
        {
            "core/tests/test_a.c": " * covers: AAA-1,AAA-2\n",
            "gen/tests/test_b.py": '"""Docstring.\n\ncovers: [AAA-1] [AAA-2]\n"""\n',
        },
    )
    report = spec_coverage.check(root)
    assert report.problems == []
    assert report.covered["AAA-2"] == ["core/tests/test_a.c:1", "gen/tests/test_b.py:3"]


def test_strings_and_fixtures_are_not_markers(tmp_path):
    root = tree(
        tmp_path,
        {
            "spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2]\ncases: []\n",
            "gen/tests/test_b.py": 'TEXT = "covers: [ZZZ-1]"\nx = """\ncovers: ZZZ-2 and more words\n"""\n',
            "core/tests/fixtures/vectors/crc/fixture.yaml": "covers: [ZZZ-3]\ncases: []\n",
            "core/tests/vendor/unity/unity.c": "/* covers: ZZZ-4 */\n",
        },
    )
    assert problems(root) == []


def test_pending_ids_are_allowed_until_covered(tmp_path):
    files = {
        "spec/vectors/rcobs/a.yaml": "covers: [AAA-1]\ncases: []\n",
        "tools/spec_pending.txt": "# ID  ticket that covers it\nAAA-2  C5\n",
    }
    assert problems(tree(tmp_path, files)) == []


def test_a_pending_id_that_is_covered_must_leave_the_list(tmp_path):
    files = {
        "spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2]\ncases: []\n",
        "tools/spec_pending.txt": "AAA-2  C5\n",
    }
    assert problems(tree(tmp_path, files)) == [
        "tools/spec_pending.txt:1: AAA-2 is covered now (spec/vectors/rcobs/a.yaml); remove it"
    ]


def test_pending_lines_need_a_known_id_and_a_ticket(tmp_path):
    files = {
        "spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2]\ncases: []\n",
        "tools/spec_pending.txt": "ZZZ-1  C5\nAAA-1\n",
    }
    assert problems(tree(tmp_path, files)) == [
        "tools/spec_pending.txt:1: ZZZ-1 is not defined in spec/protocol.md",
        "tools/spec_pending.txt:2: expected `<ID> <ticket>`",
    ]


def test_the_summary_counts_covered_and_pending(tmp_path):
    files = {
        "spec/vectors/rcobs/a.yaml": "covers: [AAA-1]\ncases: []\n",
        "tools/spec_pending.txt": "AAA-2  C5\n",
    }
    assert spec_coverage.check(tree(tmp_path, files)).summary() == "spec coverage: 1 of 2 rules covered, 1 pending"


def test_the_repository_passes():
    assert spec_coverage.check(Path(__file__).resolve().parent.parent).problems == []


DESIGN = "| C5 | Framer | C2 | x |\n| S4 | Vectors | S1 | x |\n"


def test_only_the_test_roots_are_scanned(tmp_path):
    root = tree(
        tmp_path,
        {
            "spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2]\ncases: []\n",
            ".worktrees/other/core/tests/test_new.c": "/* covers: ZZZ-1 */\n",
            "build/x/core/tests/gen.c": "/* covers: ZZZ-2 */\n",
            "scratch/t2/tests/test_s.py": "# covers: ZZZ-3\n",
            "tests/harness/proxy.py": "# covers: ZZZ-4\n",
            "core/tests/must_fail/x.c": "/* covers: ZZZ-5 */\n",
        },
    )
    assert problems(root) == []


def test_bridge_test_packages_are_scanned(tmp_path):
    root = tree(
        tmp_path,
        {
            "spec/vectors/rcobs/a.yaml": "covers: [AAA-1]\ncases: []\n",
            "bridge/r2s_bridge_tests/test/test_x.py": "# covers: AAA-2\n",
        },
    )
    assert problems(root) == []


def test_marker_shaped_lines_inside_python_strings_are_not_markers(tmp_path):
    src = 'import textwrap\n\nV = textwrap.dedent("""\\\n    covers: [AAA-2]\n    cases: []\n""")\n'
    root = tree(tmp_path, {"spec/vectors/rcobs/a.yaml": "covers: [AAA-1]\ncases: []\n", "gen/tests/test_g.py": src})
    assert "AAA-2" not in spec_coverage.check(root).covered


def test_function_docstrings_still_carry_markers(tmp_path):
    src = 'def test_x():\n    """Round trip.\n\n    covers: AAA-2\n    """\n'
    root = tree(tmp_path, {"spec/vectors/rcobs/a.yaml": "covers: [AAA-1]\ncases: []\n", "gen/tests/test_g.py": src})
    assert problems(root) == []


def test_malformed_markers_are_reported(tmp_path):
    root = tree(
        tmp_path,
        {
            "spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2]\ncases: []\n",
            "core/tests/test_a.c": "/* covers: AAA-1. */\n// covers: aaa-2\n",
        },
    )
    assert problems(root) == [
        "core/tests/test_a.c:1: malformed covers marker: 'covers: AAA-1.'",
        "core/tests/test_a.c:2: malformed covers marker: 'covers: aaa-2'",
    ]


def test_unreadable_files_are_one_line_each(tmp_path):
    root = tree(tmp_path, {"spec/vectors/rcobs/a.yaml": "covers: AAA-1\ncases: []\n"})
    (root / "core/tests").mkdir(parents=True)
    (root / "core/tests/blob.h").write_bytes(b"\xff\xfe")
    (root / "spec/vectors/rcobs/b.yaml").write_text("covers: [AAA-2\n")
    found = problems(root)
    assert any(p.startswith("core/tests/blob.h: unreadable") for p in found), found
    assert any(p.startswith("spec/vectors/rcobs/b.yaml: unreadable") for p in found), found
    assert any(p == "spec/vectors/rcobs/a.yaml: covers must be a list of spec IDs" for p in found), found


def test_pending_entries_must_be_unique_live_and_name_a_design_ticket(tmp_path):
    files = {
        "spec/vectors/rcobs/a.yaml": "covers: [AAA-1]\ncases: []\n",
        "docs/00-design.md": DESIGN,
        "tools/spec_pending.txt": "AAA-2  C5\nAAA-2  S4\nBBB-1  C5\n",
    }
    assert problems(tree(tmp_path, files)) == [
        "tools/spec_pending.txt:2: AAA-2 is listed twice",
        "tools/spec_pending.txt:3: BBB-1 is retired; remove it",
    ]
    (tmp_path / "tools/spec_pending.txt").write_text("AAA-2  nonsense\n")
    assert problems(tmp_path) == ["tools/spec_pending.txt:1: nonsense is not a ticket in docs/00-design.md"]


def test_a_covered_pending_rule_names_where_it_is_covered(tmp_path):
    files = {
        "spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2]\ncases: []\n",
        "tools/spec_pending.txt": "AAA-2  C5\n",
    }
    assert problems(tree(tmp_path, files)) == [
        "tools/spec_pending.txt:1: AAA-2 is covered now (spec/vectors/rcobs/a.yaml); remove it"
    ]


def _git(repo: Path, *args: str) -> None:
    import subprocess

    subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@example.com", *args], cwd=repo, check=True, capture_output=True
    )


def test_only_rules_new_to_the_spec_may_enter_the_pending_list(tmp_path):
    root = tree(tmp_path, {"spec/vectors/rcobs/a.yaml": "covers: [AAA-1, AAA-2]\ncases: []\n"})
    _git(root, "init", "-q")
    _git(root, "add", ".")
    _git(root, "commit", "-q", "-m", "base")
    (root / "spec/protocol.md").write_text(SPEC + "- [CCC-1] A new rule.\n")
    (root / "spec/vectors/rcobs/a.yaml").write_text("covers: [AAA-1]\ncases: []\n")
    (root / "tools").mkdir()
    (root / "tools/spec_pending.txt").write_text("AAA-2  C5\nCCC-1  C5\n")
    assert spec_coverage.check(root, base="HEAD").problems == [
        "tools/spec_pending.txt:1: AAA-2 was already a rule at HEAD; cover it instead of listing it as pending"
    ]
