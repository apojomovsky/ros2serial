"""The takeoff ritual, the git hooks and the push helper, on throwaway repositories."""

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EM = "\u2014"


def git(repo: Path, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=repo, input=stdin, capture_output=True, text=True, check=True)


def run(args, cwd, env_extra=None, stdin=None):
    env = {**os.environ, **(env_extra or {})}
    return subprocess.run(args, cwd=cwd, input=stdin, capture_output=True, text=True, env=env)


def commit(repo: Path, files: dict[str, str], message: str) -> None:
    for name, text in files.items():
        (repo / name).parent.mkdir(parents=True, exist_ok=True)
        (repo / name).write_text(text)
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", message)


def fixture(tmp_path: Path) -> tuple[Path, Path]:
    """A bare origin, a main clone on master, and a worktree on a feature branch."""
    origin = tmp_path / "origin.git"
    git(tmp_path, "init", "-q", "--bare", "-b", "master", str(origin))
    main = tmp_path / "main"
    main.mkdir()
    git(main, "init", "-q", "-b", "master")
    git(main, "config", "user.email", "t@example.com")
    git(main, "config", "user.name", "t")
    git(main, "remote", "add", "origin", str(origin))
    commit(main, {"README.md": "# x\n", ".gitignore": ".worktrees/\n"}, "chore: start")
    git(main, "push", "-q", "origin", "master")
    git(main, "worktree", "add", "-q", ".worktrees/feat", "-b", "feat/1-thing")
    return main, main / ".worktrees" / "feat"


def ritual(cwd: Path, **env) -> subprocess.CompletedProcess:
    return run(["bash", str(ROOT / "scripts/pre-pr-check.sh")], cwd, {"BASE": "origin/master", **env})


GOOD_BODY = "Closes #1\n\nThe thing, and why.\n\n## Review\n\nReviewer agent: no findings.\n"


def test_a_clean_pr_passes(tmp_path):
    main, wt = fixture(tmp_path)
    commit(wt, {"core/a.py": "# Bounded so the PIC stack never overflows.\nX = 1\n"}, "feat(core): add a thing")
    git(main, "config", "core.hooksPath", ".githooks")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)
    result = ritual(wt, PR_BODY=str(body))
    assert result.returncode == 0, result.stdout + result.stderr


def test_a_dirty_pr_gets_one_fix_line_per_problem(tmp_path):
    main, _ = fixture(tmp_path)
    long_block = "".join(f"# reason line {i}\n" for i in range(9))
    commit(main, {"docs/plans/p.md": "plan\n", "core/b.py": long_block + "Y = 2\n"}, f"feat(core): b {EM} c")
    commit(main, {"core/c.py": "Z = 3\n"}, "fix(core): c\n\nCo-Authored-By: Someone <s@example.com>")
    (main / "stray.txt").write_text("uncommitted\n")
    body = tmp_path / "body.md"
    body.write_text("Fixes things\\n- one\\n- two\n")
    result = ritual(main, PR_BODY=str(body))
    out = result.stdout
    assert result.returncode == 1, out
    for expected in [
        "working tree is not clean",
        "not in a .worktrees/ worktree",
        "on master",
        "plan file",
        "em-dash",
        "trailer",
        "9 lines",
        "literal \\n",
        "Closes #",
        "## Review",
        "hooks are not installed",
    ]:
        assert expected in out, f"{expected!r} missing from:\n{out}"


def test_spec_without_vectors_is_flagged(tmp_path):
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    commit(wt, {"spec/protocol.md": "# spec\n"}, "docs(spec): start the spec")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)
    result = ritual(wt, PR_BODY=str(body))
    assert result.returncode == 1
    assert "spec/protocol.md changed without spec/vectors" in result.stdout


def test_commit_msg_hook_rejects_a_trailer(tmp_path):
    msg = tmp_path / "MSG"
    msg.write_text("fix(core): a\n\nSigned-off-by: Someone <s@example.com>\n")
    assert run(["bash", str(ROOT / ".githooks/commit-msg"), str(msg)], tmp_path).returncode != 0
    msg.write_text("fix(core): a\n")
    assert run(["bash", str(ROOT / ".githooks/commit-msg"), str(msg)], tmp_path).returncode == 0


def pre_push(wt: Path, stdin: str, **env) -> subprocess.CompletedProcess:
    return run(["bash", str(ROOT / ".githooks/pre-push"), "origin", "url"], wt, env, stdin)


def test_pre_push_refuses_a_force_push_unless_approved(tmp_path):
    main, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    old = git(wt, "rev-parse", "HEAD").stdout.strip()
    git(wt, "push", "-q", "origin", "feat/1-thing")
    git(wt, "reset", "-q", "--hard", "HEAD~1")
    commit(wt, {"b.txt": "b\n"}, "feat(core): b")
    new = git(wt, "rev-parse", "HEAD").stdout.strip()
    line = f"refs/heads/feat/1-thing {new} refs/heads/feat/1-thing {old}\n"
    refused = pre_push(wt, line, R2S_PRE_PUSH_SUITE="true")
    assert refused.returncode != 0
    assert "force push" in refused.stdout
    assert pre_push(wt, line, R2S_PRE_PUSH_SUITE="true", R2S_FORCE_PUSH_APPROVED="1").returncode == 0


def test_pre_push_runs_the_suite_once_per_tree(tmp_path):
    _, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    head = git(wt, "rev-parse", "HEAD").stdout.strip()
    line = f"refs/heads/feat/1-thing {head} refs/heads/feat/1-thing {'0' * 40}\n"
    log = tmp_path / "suite.log"
    suite = f"echo ran >> {log}"
    assert pre_push(wt, line, R2S_PRE_PUSH_SUITE=suite).returncode == 0
    assert pre_push(wt, line, R2S_PRE_PUSH_SUITE=suite).returncode == 0
    assert log.read_text().split() == ["ran"]


def test_push_helper_verifies_the_remote_ref(tmp_path):
    _, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    result = run(["bash", str(ROOT / "scripts/push.sh")], wt, {"R2S_PRE_PUSH_SUITE": "true"})
    assert result.returncode == 0, result.stdout + result.stderr
    remote = git(wt, "ls-remote", "origin", "refs/heads/feat/1-thing").stdout.split()[0]
    assert remote == git(wt, "rev-parse", "HEAD").stdout.strip()


def test_push_helper_forbids_no_verify(tmp_path):
    _, wt = fixture(tmp_path)
    assert run(["bash", str(ROOT / "scripts/push.sh"), "--no-verify"], wt).returncode == 2


def test_an_unreadable_pr_body_is_a_fix_not_a_silent_pass(tmp_path):
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    body = tmp_path / "body.md"
    body.write_bytes(b"Closes #1\n\xff\xfe\n")
    result = ritual(wt, PR_BODY=str(body))
    assert result.returncode == 1
    assert "PR body" in result.stdout


def test_commit_msg_hook_lets_git_merge_messages_through_during_a_merge(tmp_path):
    main, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    msg = tmp_path / "MSG"
    msg.write_text("Merge branch 'ci/8-hooks' into feat/1-thing\n")
    assert run(["bash", str(ROOT / ".githooks/commit-msg"), str(msg)], wt).returncode != 0
    merge_head = Path(git(wt, "rev-parse", "--git-path", "MERGE_HEAD").stdout.strip())
    if not merge_head.is_absolute():
        merge_head = wt / merge_head
    merge_head.write_text(git(wt, "rev-parse", "HEAD").stdout)
    assert run(["bash", str(ROOT / ".githooks/commit-msg"), str(msg)], wt).returncode == 0
    msg.write_text("Merge branch 'x'\n\nSigned-off-by: Someone <s@example.com>\n")
    assert run(["bash", str(ROOT / ".githooks/commit-msg"), str(msg)], wt).returncode != 0


def test_pre_push_runs_the_suite_without_the_hooks_git_variables(tmp_path):
    _, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    head = git(wt, "rev-parse", "HEAD").stdout.strip()
    line = f"refs/heads/feat/1-thing {head} refs/heads/feat/1-thing {'0' * 40}\n"
    gitdir = git(wt, "rev-parse", "--absolute-git-dir").stdout.strip()
    out = tmp_path / "suite-env.txt"
    # Git exports GIT_DIR to a pre-push hook run from a worktree; the suite must not inherit it.
    assert pre_push(wt, line, R2S_PRE_PUSH_SUITE=f"env > {out}", GIT_DIR=gitdir).returncode == 0
    assert "GIT_DIR=" not in out.read_text()


def test_commit_msg_hook_still_checks_other_subjects_during_a_merge(tmp_path):
    _, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    merge_head = Path(git(wt, "rev-parse", "--absolute-git-dir").stdout.strip()) / "MERGE_HEAD"
    merge_head.write_text(git(wt, "rev-parse", "HEAD").stdout)
    msg = tmp_path / "MSG"
    msg.write_text("wip stuff\n")
    assert run(["bash", str(ROOT / ".githooks/commit-msg"), str(msg)], wt).returncode != 0
    # merge.log=true appends a shortlog; git's own merge message may be longer than 3 lines.
    msg.write_text("Merge branch 'x'\n\n* x:\n  feat(core): a\n  feat(core): b\n")
    assert run(["bash", str(ROOT / ".githooks/commit-msg"), str(msg)], wt).returncode == 0


def test_the_suite_cleans_the_git_variables_before_any_section():
    text = (ROOT / "scripts/ci-test.sh").read_text()
    assert text.index('source "$root/scripts/clean-git-env.sh"') < text.index("\nif wants ")
    hook = (ROOT / ".githooks/pre-push").read_text()
    assert hook.index('source "$root/scripts/clean-git-env.sh"') < hook.index("R2S_PRE_PUSH_SUITE")


def test_the_ritual_does_not_leak_its_scopes_into_the_suite(tmp_path):
    """A ritual that runs the suite must not let that suite run a ritual.

    The suite's own tests call the ritual, so a scope that reaches the suite
    comes back as another ritual, and the pair forks without bound. The
    ritual therefore starts the suite with the scopes cleared.
    """
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    commit(wt, {"core/a.py": "# Bounded so the PIC stack never overflows.\nX = 1\n"}, "feat(core): add a thing")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)

    # The suite records the scopes it was started with, so a leak shows.
    marker = tmp_path / "scopes.txt"
    recorder = tmp_path / "record-scopes.sh"
    recorder.write_text(f'echo "TEST=${{TEST:-}} SCRIPTS=${{SCRIPTS:-}}" > {marker}\n')

    result = ritual(wt, TEST="1", SCRIPTS="1", PR_BODY=str(body), R2S_SUITE=str(recorder))
    # The exit code is not the point: the copy of the tree has no compiler
    # toolchain, so the ritual reports other problems. What matters is what
    # the suite was given.
    assert marker.exists(), result.stdout + result.stderr
    # TEST is the trigger: the suite's own tests call the ritual, and a
    # ritual that still sees TEST=1 runs the suite again.
    assert marker.read_text().strip() == "TEST= SCRIPTS=", (
        "the suite was started with the scope that makes it run a ritual again"
    )
