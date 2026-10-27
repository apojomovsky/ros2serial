"""Check 8 of the ritual: a wire rule and its vector change together.

A section whose heading ends in `(local)` specifies bytes that never appear
on the wire, so its rules are checked by tests; a rule in any other section
crosses the wire, and a change to one needs a change under `spec/vectors`.
"""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import spec_sync_check  # noqa: E402

LOCAL = """# spec

## 1. Wire

- [FRM-1] A frame is bytes.

## 9. Descriptors (local)

- [DSC-1] A type record is 5 bytes.

### 9.1 Storage

- [DSC-2] An offset is a byte count.
"""


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True)


def repo_with(tmp_path: Path, text: str, files: dict[str, str] | None = None) -> Path:
    (tmp_path / "spec").mkdir(parents=True, exist_ok=True)
    (tmp_path / "spec/protocol.md").write_text(text)
    for name, content in (files or {}).items():
        (tmp_path / name).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / name).write_text(content)
    git(tmp_path, "init", "-q")
    git(tmp_path, "config", "user.email", "t@example.com")
    git(tmp_path, "config", "user.name", "t")
    git(tmp_path, "add", ".")
    git(tmp_path, "commit", "-q", "-m", "base")
    return tmp_path


def with_wire_rule(text: str) -> str:
    """A new wire rule, in the wire section rather than under the local one."""
    return text.replace("## 9. Descriptors (local)", "- [FRM-2] A frame has a topic.\n\n## 9. Descriptors (local)")


def write(root: Path, name: str, text: str) -> None:
    (root / name).parent.mkdir(parents=True, exist_ok=True)
    (root / name).write_text(text)
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "change")


def problems(root: Path) -> list[str]:
    # The base commit is the one repo_with made; the change is on top of it.
    return spec_sync_check.check(root, "HEAD~1")


def test_a_wire_rule_added_without_a_vector_fails(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/protocol.md", with_wire_rule(LOCAL))
    assert problems(root) == ["spec/protocol.md changed without spec/vectors changes: FRM-2 crosses the wire"]


def test_a_wire_rule_reworded_without_a_vector_fails(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/protocol.md", LOCAL.replace("A frame is bytes.", "A frame is some bytes."))
    assert problems(root) == ["spec/protocol.md changed without spec/vectors changes: FRM-1 crosses the wire"]


def test_a_local_rule_added_without_a_vector_passes(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/protocol.md", LOCAL + "- [DSC-3] A count is a byte count.\n")
    assert problems(root) == []


def test_a_local_rule_reworded_without_a_vector_passes(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/protocol.md", LOCAL.replace("5 bytes", "6 bytes"))
    assert problems(root) == []


def test_a_subsection_inherits_the_local_marker(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/protocol.md", LOCAL + "\n### 9.2 Counts\n\n- [DSC-4] A count is a number.\n")
    assert problems(root) == []


def test_a_wire_rule_moved_into_a_local_section_fails(tmp_path):
    """Relabelling a wire rule as local takes it off the wire without a vector."""
    base = LOCAL.replace(
        "- [FRM-1] A frame is bytes.\n", "- [FRM-1] A frame is bytes.\n- [FRM-2] A frame has a topic.\n"
    )
    root = repo_with(tmp_path, base)
    moved = base.replace("- [FRM-2] A frame has a topic.\n", "").replace(
        "- [DSC-1] A type record is 5 bytes.\n",
        "- [DSC-1] A type record is 5 bytes.\n- [FRM-2] A frame has a topic.\n",
    )
    write(root, "spec/protocol.md", moved)
    assert problems(root) == ["spec/protocol.md changed without spec/vectors changes: FRM-2 crosses the wire"]


def test_a_section_after_the_local_one_is_wire_again(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/protocol.md", LOCAL + "\n## 10. Session\n\n- [SES-1] A session starts.\n")
    assert problems(root) == ["spec/protocol.md changed without spec/vectors changes: SES-1 crosses the wire"]


def test_prose_changed_without_a_vector_passes(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/protocol.md", LOCAL.replace("# spec\n", "# spec\n\nAn introduction.\n"))
    assert problems(root) == []


def test_a_wire_rule_with_a_vector_change_passes(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    (root / "spec/vectors/frames/a.yaml").parent.mkdir(parents=True)
    (root / "spec/vectors/frames/a.yaml").write_text("covers: [FRM-2]\ncases: []\n")
    (root / "spec/protocol.md").write_text(with_wire_rule(LOCAL))
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "change")
    assert problems(root) == []


def test_a_vector_covering_a_known_rule_without_a_spec_change_passes(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/vectors/frames/a.yaml", "covers: [FRM-1]\ncases: []\n")
    assert problems(root) == []


def test_a_vector_naming_an_unknown_rule_without_a_spec_change_fails(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/vectors/frames/a.yaml", "covers: [FRM-9]\ncases: []\n")
    assert problems(root) == ["spec/vectors changed without spec/protocol.md changes: FRM-9 names no rule"]


def test_a_vector_without_checkable_covers_without_a_spec_change_fails(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "spec/vectors/frames/a.yaml", "name: a\n")
    assert problems(root) == [
        "spec/vectors changed without spec/protocol.md changes: spec/vectors/frames/a.yaml names no rule"
    ]


def test_deleting_a_vector_needs_no_spec_change(tmp_path):
    root = repo_with(tmp_path, LOCAL, {"spec/vectors/frames/a.yaml": "covers: [FRM-1]\n"})
    git(root, "rm", "-q", "spec/vectors/frames/a.yaml")
    git(root, "commit", "-q", "-m", "change")
    assert problems(root) == []


def test_a_branch_that_changes_neither_passes(tmp_path):
    root = repo_with(tmp_path, LOCAL)
    write(root, "core.c", "int x;\n")
    assert problems(root) == []


def test_removing_a_vector_placeholder_needs_no_spec_change(tmp_path):
    """A .gitkeep carries no vector, so dropping one is not a vector change."""
    root = repo_with(tmp_path, LOCAL, {"spec/vectors/crc/.gitkeep": ""})
    git(root, "rm", "-q", "spec/vectors/crc/.gitkeep")
    git(root, "commit", "-q", "-m", "change")
    assert problems(root) == []


def test_a_wire_rule_with_only_a_placeholder_change_fails(tmp_path):
    root = repo_with(tmp_path, LOCAL, {"spec/vectors/crc/.gitkeep": ""})
    git(root, "rm", "-q", "spec/vectors/crc/.gitkeep")
    write(root, "spec/protocol.md", with_wire_rule(LOCAL))
    assert problems(root) == ["spec/protocol.md changed without spec/vectors changes: FRM-2 crosses the wire"]
