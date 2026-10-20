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


def test_repository_matches_the_layout():
    root = Path(check_layout.__file__).resolve().parent.parent
    assert check_layout.check(root) == []
