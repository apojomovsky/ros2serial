from pathlib import Path

import check_layout


def make_skeleton(root: Path) -> None:
    for d in check_layout.REQUIRED_DIRS:
        (root / d).mkdir(parents=True, exist_ok=True)
    for f in check_layout.REQUIRED_FILES:
        (root / f).parent.mkdir(parents=True, exist_ok=True)
        (root / f).write_text("")
    (root / "LICENSE").write_text("MIT License\n")
    (root / "AGENTS.md").write_text("See docs/00-design.md\n")
    (root / "CONTRIBUTING.md").write_text("AGENTS.md is the rulebook\n")
    (root / ".gitignore").write_text("scratch/\n.worktrees/\n")
    # The alias is a link, so the rulebook it points at cannot drift.
    (root / "CLAUDE.md").unlink(missing_ok=True)
    (root / "CLAUDE.md").symlink_to("AGENTS.md")


def test_complete_skeleton_passes(tmp_path):
    make_skeleton(tmp_path)
    assert check_layout.check(tmp_path) == []


def test_missing_directory_is_named(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / "core/fuzz").rmdir()
    assert check_layout.check(tmp_path) == ["missing directory: core/fuzz"]


def test_rulebook_links_are_required(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / "AGENTS.md").write_text("no link\n")
    (tmp_path / "CONTRIBUTING.md").write_text("no deferral\n")
    assert check_layout.check(tmp_path) == [
        "AGENTS.md does not link docs/00-design.md",
        "CONTRIBUTING.md does not defer to AGENTS.md",
    ]


def test_superseded_documents_are_flagged(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / "proposal-v3.md").write_text("")
    (tmp_path / "engineering-practices.md").write_text("")
    assert check_layout.check(tmp_path) == [
        "superseded document still at the root: engineering-practices.md",
        "superseded document still at the root: proposal-v3.md",
    ]


def test_gitignore_entries_are_required(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / ".gitignore").write_text("scratch/\n")
    assert check_layout.check(tmp_path) == [".gitignore does not ignore .worktrees/"]


def test_the_gate_asks_for_the_design_and_the_testing_rules(tmp_path):
    """The gate asks for what a contributor needs, and nothing else."""
    make_skeleton(tmp_path)
    assert check_layout.check(tmp_path) == []
    assert [d for d in check_layout.REQUIRED_DIRS if d.startswith("docs/")] == []
    assert [f for f in check_layout.REQUIRED_FILES if f.startswith("docs/")] == [
        "docs/00-design.md",
        "docs/01-testing.md",
    ]


def test_the_testing_rules_are_required(tmp_path):
    """The testing strategy is normative, so the gate must ask for it."""
    make_skeleton(tmp_path)
    (tmp_path / "docs/01-testing.md").unlink()
    assert check_layout.check(tmp_path) == ["missing file: docs/01-testing.md"]


def test_the_rulebook_alias_must_exist(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / "CLAUDE.md").unlink()
    assert check_layout.check(tmp_path) == ["missing file: CLAUDE.md"]


def test_the_rulebook_alias_must_be_a_link(tmp_path):
    """A copy of the rulebook would drift from it, so only a link passes."""
    make_skeleton(tmp_path)
    (tmp_path / "CLAUDE.md").unlink()
    (tmp_path / "CLAUDE.md").write_text("See docs/00-design.md\n")
    assert check_layout.check(tmp_path) == ["CLAUDE.md is not a symlink to AGENTS.md"]


def test_the_rulebook_alias_must_point_at_the_rulebook(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / "CLAUDE.md").unlink()
    (tmp_path / "CLAUDE.md").symlink_to("README.md")
    assert check_layout.check(tmp_path) == ["CLAUDE.md is not a symlink to AGENTS.md"]


def test_a_link_that_points_at_itself_is_reported_not_raised(tmp_path):
    """A loop must be a problem line, not an exception that stops the gate."""
    make_skeleton(tmp_path)
    (tmp_path / "CLAUDE.md").unlink()
    (tmp_path / "CLAUDE.md").symlink_to("CLAUDE.md")
    assert check_layout.check(tmp_path) == [
        "missing file: CLAUDE.md",
        "CLAUDE.md is not a symlink to AGENTS.md",
    ]


def test_repository_matches_the_layout():
    root = Path(check_layout.__file__).resolve().parent.parent
    assert check_layout.check(root) == []
