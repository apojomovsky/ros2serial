"""The takeoff ritual, the git hooks and the push helper, on throwaway repositories."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import commit_check  # noqa: E402

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
    # The ritual takes its title and its body from the environment, so a
    # value in the caller's shell would be checked against the fixture
    # commits of the test. Only what the test passes is the ritual's input.
    inherited = {k: v for k, v in os.environ.items() if k not in ("PR_TITLE", "PR_BODY")}
    return subprocess.run(
        ["bash", str(ROOT / "scripts/pre-pr-check.sh")],
        cwd=cwd,
        capture_output=True,
        text=True,
        env={**inherited, "BASE": "origin/master", **env},
    )


GOOD_BODY = "Closes #1\n\nThe thing, and why.\n\n## Review\n\nReviewer agent: no findings.\n"


def test_a_clean_pr_passes(tmp_path):
    main, wt = fixture(tmp_path)
    commit(wt, {"core/a.py": "# Bounded so the PIC stack never overflows.\nX = 1\n"}, "feat(core): add a thing")
    git(main, "config", "core.hooksPath", ".githooks")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)
    result = ritual(wt, PR_BODY=str(body))
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_ritual_checks_the_title_against_the_commits(tmp_path):
    """The title is the subject that lands, so the ritual must check it.

    The check is the one CI runs, so an agent finds the problem before the
    PR is opened instead of after.
    """
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    commit(wt, {"core/a.py": "# Bounded so the PIC stack never overflows.\nX = 1\n"}, "fix(core): repair the thing")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)

    weak = ritual(wt, PR_BODY=str(body), PR_TITLE="docs(ci): describe the thing")
    assert weak.returncode == 1, weak.stdout + weak.stderr
    assert "the title is 'docs' but a commit is 'fix'" in weak.stdout

    strong = ritual(wt, PR_BODY=str(body), PR_TITLE="fix(ci): repair the thing")
    assert strong.returncode == 0, strong.stdout + strong.stderr


def test_the_ritual_says_the_title_was_not_checked(tmp_path):
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    commit(wt, {"core/a.py": "# Bounded so the PIC stack never overflows.\nX = 1\n"}, "fix(core): repair the thing")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)

    out = ritual(wt, PR_BODY=str(body))
    assert out.returncode == 0, out.stdout + out.stderr
    assert "PR title" in out.stdout
    assert "not checked" in out.stdout


def test_the_ritual_ignores_a_title_in_the_callers_shell(tmp_path, monkeypatch):
    """A title in the caller's shell is not the ritual's input.

    The suite runs with the caller's environment, and `make test PR_TITLE=...`
    is how the same check CI runs is tried locally, so an ambient title would
    be checked against the fixture commits of these tests.
    """
    monkeypatch.setenv("PR_TITLE", "docs(ci): a title for another branch")
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    commit(wt, {"core/a.py": "# Bounded so the PIC stack never overflows.\nX = 1\n"}, "fix(core): repair the thing")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)

    out = ritual(wt, PR_BODY=str(body))
    assert out.returncode == 0, out.stdout + out.stderr
    assert "not checked" in out.stdout


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


def test_a_plan_file_anywhere_is_flagged(tmp_path):
    """Plans do not belong in the tree, whatever directory they are written in."""
    _, wt = fixture(tmp_path)
    commit(
        wt,
        {"docs/plans/a.md": "plan\n", "docs/plan.md": "plan\n", "src/plan/z.md": "plan\n"},
        "docs(ci): start a plan",
    )
    result = ritual(wt)
    assert result.returncode == 1, result.stdout + result.stderr
    for path in ["docs/plans/a.md", "docs/plan.md", "src/plan/z.md"]:
        assert f"plan file in the diff: {path}" in result.stdout


def test_spec_without_vectors_is_flagged(tmp_path):
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    commit(wt, {"spec/protocol.md": "# spec\n\n- [FRM-1] A frame is bytes.\n"}, "docs(spec): start the spec")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)
    result = ritual(wt, PR_BODY=str(body))
    assert result.returncode == 1
    assert "spec/protocol.md changed without spec/vectors" in result.stdout


def test_a_vector_change_without_the_spec_is_flagged(tmp_path):
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    commit(wt, {"spec/vectors/crc/values.yaml": "cases: []\n"}, "test(spec): add a vector")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)
    result = ritual(wt, PR_BODY=str(body))
    assert result.returncode == 1
    assert "spec/vectors changed without spec/protocol.md changes" in result.stdout


def test_a_placeholder_is_not_a_vector_change(tmp_path):
    """A .gitkeep carries no vector, so removing one keeps the spec in sync."""
    main, wt = fixture(tmp_path)
    commit(main, {"spec/vectors/crc/.gitkeep": ""}, "chore(spec): keep the vector directory")
    git(main, "push", "-q", "origin", "master")
    git(wt, "merge", "-q", "--no-edit", "origin/master")
    git(wt, "rm", "-q", "spec/vectors/crc/.gitkeep")
    git(wt, "commit", "-q", "-m", "chore(spec): drop the vector placeholder")
    git(main, "config", "core.hooksPath", ".githooks")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)
    result = ritual(wt, PR_BODY=str(body))
    assert result.returncode == 0, result.stdout + result.stderr


def test_a_local_spec_change_needs_no_vector(tmp_path):
    """A section marked local never appears on the wire, so tests cover it."""
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    spec = "# spec\n\n## 9. Descriptors (local)\n\n- [DSC-1] A type record is 5 bytes.\n"
    commit(wt, {"spec/protocol.md": spec}, "docs(spec): describe the tables")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)
    result = ritual(wt, PR_BODY=str(body))
    assert result.returncode == 0, result.stdout + result.stderr
    assert "without spec/vectors" not in result.stdout


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
    """A branch catches up by merging master in, and git writes that message."""
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


def test_a_non_conventional_subject_outside_a_merge_still_fails(tmp_path):
    _, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    msg = tmp_path / "MSG"
    msg.write_text("wip stuff\n")
    assert run(["bash", str(ROOT / ".githooks/commit-msg"), str(msg)], wt).returncode != 0


def test_a_branch_behind_its_base_is_told_to_merge_it_in(tmp_path):
    """A branch whose files overlap master's must merge it in first."""
    main, wt = fixture(tmp_path)
    commit(wt, {"shared.txt": "branch\n"}, "feat(core): a")
    commit(main, {"shared.txt": "master\n"}, "chore: master moves on")
    git(main, "push", "-q", "origin", "master")
    git(wt, "fetch", "-q", "origin")
    out = ritual(wt).stdout
    assert "behind origin/master" in out
    assert "merge origin/master" in out
    assert "rebase" not in out
    assert "R2S_FORCE_PUSH_APPROVED" not in out


def test_the_range_check_skips_a_merged_in_master(tmp_path):
    main, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    commit(main, {"b.txt": "b\n"}, "chore: master moves on")
    git(main, "push", "-q", "origin", "master")
    git(wt, "fetch", "-q", "origin")
    git(wt, "merge", "-q", "--no-edit", "origin/master")
    assert git(wt, "log", "--merges", "--format=%s").stdout.startswith("Merge ")
    result = run(["python3", str(ROOT / "scripts/commit_check.py"), "--range", "origin/master..HEAD"], wt)
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_suite_cleans_the_git_variables_before_any_section():
    text = (ROOT / "scripts/ci-test.sh").read_text()
    assert text.index('source "$root/scripts/clean-git-env.sh"') < text.index("\nif wants ")
    hook = (ROOT / ".githooks/pre-push").read_text()
    assert hook.index('source "$root/scripts/clean-git-env.sh"') < hook.index("R2S_PRE_PUSH_SUITE")


def test_the_ritual_does_not_leak_its_scopes_into_the_suite(tmp_path):
    """A ritual that runs the suite must not let that suite run a ritual.

    The suite's own tests call the ritual, so a TEST that reaches the suite
    comes back as another ritual, and the pair forks without bound. The
    ritual therefore starts the suite without it.
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
    # The exit code is not the point: the fixture is a bare repository with
    # no compiler toolchain, so the ritual reports other problems. Every
    # test here asserts on stdout for the same reason. What matters is what
    # the suite was given.
    assert marker.exists(), result.stdout + result.stderr
    # TEST is the trigger: the suite's own tests call the ritual, and a
    # ritual that still sees TEST=1 runs the suite again. The scopes stay,
    # so the child does what this run was asked to do.
    assert marker.read_text().strip() == "TEST= SCRIPTS=1", "the suite was started with TEST still set"


def test_the_ritual_does_not_leak_its_title_into_the_suite(tmp_path):
    """The suite's own tests call the ritual, and they carry no title.

    A title that reached the suite would be checked against the fixture
    commits of those tests, and the ritual would fail for a title that is
    perfectly good.
    """
    main, wt = fixture(tmp_path)
    git(main, "config", "core.hooksPath", ".githooks")
    commit(wt, {"core/a.py": "# Bounded so the PIC stack never overflows.\nX = 1\n"}, "feat(core): add a thing")
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)

    marker = tmp_path / "title.txt"
    recorder = tmp_path / "record-title.sh"
    recorder.write_text(f'echo "PR_TITLE=${{PR_TITLE:-}}" > {marker}\n')

    ritual(wt, TEST="1", PR_BODY=str(body), PR_TITLE="feat(core): add a thing", R2S_SUITE=str(recorder))
    assert marker.exists()
    assert marker.read_text().strip() == "PR_TITLE=", "the suite was started with the outer PR_TITLE"


def test_the_title_must_carry_the_strongest_type_of_its_commits():
    """The title is the subject that lands, so it must carry the type.

    A PR holding a feat commit but titled docs(...) reaches neither the
    changelog nor the version bump, and a `!` on a commit is lost with the
    commit.
    """
    assert commit_check.check_title_covers("docs(core): add the encoder", ["feat(core): add the encoder"]) != []
    assert commit_check.check_title_covers("feat(core): add the encoder", ["feat(core): add the encoder"]) == []
    assert commit_check.check_title_covers("fix(core): a thing", ["fix(core): a thing"]) == []
    assert commit_check.check_title_covers("docs(spec): tidy", ["docs(spec): tidy"]) == []
    # A breaking change on a commit is lost unless the title carries it.
    assert commit_check.check_title_covers("feat(core): widen ids", ["feat(spec)!: widen ids"]) != []
    assert commit_check.check_title_covers("feat(core)!: widen ids", ["feat(spec)!: widen ids"]) == []


def test_the_merge_settings_check_catches_a_wrong_setting(tmp_path):
    """The settings are what make the rule hold, so they are checked."""
    fake = tmp_path / "bin"
    fake.mkdir()
    gh = fake / "gh"
    gh.write_text(
        "#!/usr/bin/env bash\n"
        'if [ "$1" = "api" ]; then printf "true|true|true|COMMIT_MESSAGES|COMMIT_MESSAGES\\n"; exit 0; fi\n'
        'echo "apojomovsky/ros2serial"\n'
    )
    gh.chmod(0o755)
    env = {**os.environ, "PATH": f"{fake}{os.pathsep}{os.environ['PATH']}"}
    out = run(["bash", str(ROOT / "scripts/merge-settings.sh")], ROOT, env)
    assert out.returncode == 1
    assert "do not match the squash rule" in out.stderr


def test_settings_that_cannot_be_read_are_not_a_failure(tmp_path):
    """gh refuses to run without a token, which is not a wrong setting.

    The repository has no secret yet, so a workflow without one cannot read
    the settings and says so.
    """
    fake = tmp_path / "bin"
    fake.mkdir()
    gh = fake / "gh"
    gh.write_text(
        "#!/usr/bin/env bash\n"
        'if [ "$1" = "api" ]; then echo "gh: set the GH_TOKEN environment variable." >&2; exit 4; fi\n'
        'echo "apojomovsky/ros2serial"\n'
    )
    gh.chmod(0o755)
    env = {**os.environ, "PATH": f"{fake}{os.pathsep}{os.environ['PATH']}", "GH_TOKEN": ""}
    out = run(["bash", str(ROOT / "scripts/merge-settings.sh")], ROOT, env)
    assert out.returncode == 75
    assert "cannot be read" in out.stderr


def test_settings_that_are_not_visible_to_the_token_are_not_a_failure(tmp_path):
    """The API hides the merge settings from a token that is not an admin.

    A fork's pull request has no token that can read them, and a token that
    cannot is not a wrong setting.
    """
    fake = tmp_path / "bin"
    fake.mkdir()
    gh = fake / "gh"
    gh.write_text(
        '#!/usr/bin/env bash\nif [ "$1" = "api" ]; then printf "||||\\n"; exit 0; fi\necho "apojomovsky/ros2serial"\n'
    )
    gh.chmod(0o755)
    env = {**os.environ, "PATH": f"{fake}{os.pathsep}{os.environ['PATH']}"}
    out = run(["bash", str(ROOT / "scripts/merge-settings.sh")], ROOT, env)
    assert out.returncode == 75
    assert "not visible" in out.stderr


def test_the_pre_push_hook_refuses_a_force_push(tmp_path):
    """Every force push needs the maintainer's go-ahead, a rebase included.

    The gate exists so that work another agent may have pulled is never
    dropped, and the maintainer keeps it. Catching up with master is a
    rebase, which is a force push, so it asks.
    """
    repo = tmp_path / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "master")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "user.name", "t")
    (repo / "a.txt").write_text("one\n")
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "feat(core): a")
    git(repo, "switch", "-q", "-c", "feat/1-thing")
    (repo / "b.txt").write_text("two\n")
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "feat(core): b")
    remote = tmp_path / "remote.git"
    git(tmp_path, "init", "-q", "--bare", str(remote))
    git(repo, "push", "-q", str(remote), "feat/1-thing")
    # The hook is installed after the first push, so the push under test is
    # the one the hook sees.
    (repo / ".githooks").mkdir()
    shutil.copy2(ROOT / ".githooks/pre-push", repo / ".githooks/pre-push")
    (repo / ".githooks/pre-push").chmod(0o755)
    git(repo, "config", "core.hooksPath", ".githooks")

    # Master moves on and the branch is rebased onto it: a force push.
    git(repo, "switch", "-q", "master")
    (repo / "c.txt").write_text("three\n")
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "chore: master moves")
    git(repo, "switch", "-q", "feat/1-thing")
    git(repo, "rebase", "-q", "master")

    env = {**os.environ, "R2S_PRE_PUSH_SUITE": "true", "R2S_FORCE_PUSH_APPROVED": ""}
    refused = subprocess.run(
        ["git", "push", "--force", str(remote), "feat/1-thing"],
        cwd=repo,
        capture_output=True,
        text=True,
        env=env,
    )
    said = refused.stdout + refused.stderr
    assert "refusing a force push" in said, said

    # With the maintainer's go-ahead it goes through.
    env = {**os.environ, "R2S_PRE_PUSH_SUITE": "true", "R2S_FORCE_PUSH_APPROVED": "1"}
    allowed = subprocess.run(
        ["git", "push", "--force", str(remote), "feat/1-thing"],
        cwd=repo,
        capture_output=True,
        text=True,
        env=env,
    )
    assert "refusing a force push" not in allowed.stdout + allowed.stderr


def _fake_gh(tmp_path: Path, settings: str) -> dict:
    fake = tmp_path / "bin"
    fake.mkdir()
    gh = fake / "gh"
    gh.write_text(
        "#!/usr/bin/env bash\n"
        f'if [ "$1" = "api" ]; then printf "%s\\n" "{settings}"; exit 0; fi\n'
        'echo "apojomovsky/ros2serial"\n'
    )
    gh.chmod(0o755)
    return {**os.environ, "PATH": f"{fake}{os.pathsep}{os.environ['PATH']}"}


def test_the_merge_settings_that_match_the_rule_pass(tmp_path):
    env = _fake_gh(tmp_path, "true|false|false|PR_TITLE|BLANK")
    out = run(["bash", str(ROOT / "scripts/merge-settings.sh")], ROOT, env)
    assert out.returncode == 0, out.stderr


def test_an_empty_setting_does_not_shift_the_ones_after_it(tmp_path):
    env = _fake_gh(tmp_path, "true|false|false||BLANK")
    out = run(["bash", str(ROOT / "scripts/merge-settings.sh")], ROOT, env)
    assert out.returncode == 1
    assert "the squash title is , not PR_TITLE" in out.stderr
    assert "squash message" not in out.stderr


def test_the_ritual_says_once_that_the_settings_were_not_checked(tmp_path):
    main, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    fake = tmp_path / "gbin"
    fake.mkdir()
    gh = fake / "gh"
    gh.write_text(
        "#!/usr/bin/env bash\n"
        'if [ "$1" = "api" ]; then echo "gh: set the GH_TOKEN environment variable." >&2; exit 4; fi\n'
        'echo "apojomovsky/ros2serial"\n'
    )
    gh.chmod(0o755)
    env = {"PATH": f"{fake}{os.pathsep}{os.environ['PATH']}", "GH_TOKEN": ""}
    out = ritual(wt, **env)
    lines = [ln for ln in (out.stdout + out.stderr).splitlines() if "merge-settings" in ln or "merge settings" in ln]
    assert len(lines) == 1, lines


def test_the_release_workflow_calls_the_subject_script():
    """The tag job's check is a script so that its test cannot drift from it."""
    text = (ROOT / ".github/workflows/cut-release.yml").read_text()
    assert "scripts/release-subject.sh" in text


def test_a_docs_title_survives_a_merged_in_master(tmp_path):
    """The title check covers the branch's own commits, not master's.

    When master moves on with a feat commit and the branch merges it in, a
    range from the old base puts master's feat inside the range and a docs
    title fails. The merge-base range holds only the branch's commits.
    """
    main, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "docs(core): describe the thing")
    old_base = git(wt, "rev-parse", "HEAD").stdout.strip()
    commit(main, {"b.txt": "b\n"}, "feat(core): a new thing")
    git(main, "push", "-q", "origin", "master")
    git(wt, "fetch", "-q", "origin")
    git(wt, "merge", "-q", "--no-edit", "origin/master")
    stale = f"{old_base}..HEAD"
    stale_result = run(
        [
            "python3",
            str(ROOT / "scripts/commit_check.py"),
            "--title",
            "docs(core): describe the thing",
            "--covers-range",
            stale,
        ],
        wt,
    )
    assert stale_result.returncode == 1, stale_result.stdout + stale_result.stderr
    stale_subjects = git(wt, "log", "--no-merges", "--format=%s", stale).stdout.splitlines()
    assert "feat(core): a new thing" in stale_subjects
    script = str(ROOT / "scripts/pr-title-range.sh")
    fresh_range = run(["bash", script], wt, {"BASE_REF": "master"}).stdout.strip()
    subjects = git(wt, "log", "--no-merges", "--format=%s", fresh_range).stdout.splitlines()
    assert subjects == ["docs(core): describe the thing"]
    fresh = run(
        [
            "python3",
            str(ROOT / "scripts/commit_check.py"),
            "--title",
            "docs(core): describe the thing",
            "--covers-range",
            fresh_range,
        ],
        wt,
    )
    assert fresh.returncode == 0, fresh.stdout + fresh.stderr
    # The merge ref can be built from the older base after the branch merged
    # the newer one, so the script must stay exact on that checkout too.
    head = git(wt, "rev-parse", "HEAD").stdout.strip()
    git(wt, "checkout", "-q", "--detach", old_base)
    git(wt, "merge", "-q", "--no-edit", "--no-ff", head, "-m", "stale merge ref")
    stale_ref_range = run(["bash", script], wt, {"BASE_REF": "master"}).stdout.strip()
    stale_ref_subjects = git(wt, "log", "--no-merges", "--format=%s", stale_ref_range).stdout.splitlines()
    assert stale_ref_subjects == ["docs(core): describe the thing"]
    stale_ref = run(
        [
            "python3",
            str(ROOT / "scripts/commit_check.py"),
            "--title",
            "docs(core): describe the thing",
            "--covers-range",
            stale_ref_range,
        ],
        wt,
    )
    assert stale_ref.returncode == 0, stale_ref.stdout + stale_ref.stderr


def test_the_ci_workflow_resolves_its_ranges_from_the_merge_base():
    """The lint and coverage ranges go stale when the base moves on.

    Both jobs built COMMIT_RANGE from the payload base.sha, which freezes
    the base tip at PR creation; a base that moves on and is merged in then
    lands inside the range. They resolve the range from the merge base with
    the current base branch instead, as the pr-title job does.
    """
    text = (ROOT / ".github/workflows/ci.yml").read_text()
    assert "pull_request.base.sha" not in text
    assert text.count('range="$(bash scripts/pr-title-range.sh)" || exit 1') == 2
    assert text.count("BASE_REF: ${{ github.event.pull_request.base.ref }}") == 2


def test_the_pr_title_workflow_uses_the_range_script():
    """The range expression is a script so that its test cannot drift from it."""
    text = (ROOT / ".github/workflows/pr-title.yml").read_text()
    assert "scripts/pr-title-range.sh" in text
    assert "BASE_REF: ${{ github.event.pull_request.base.ref }}" in text
    assert 'range="$(bash scripts/pr-title-range.sh)" || exit 1' in text
    assert "pull_request.base.sha" not in text


def test_the_pr_range_script_uses_the_merge_base_not_the_tip(tmp_path):
    """A branch behind master still gets its own commits, not the base tip.

    The merge base and the base tip diverge here, so printing the tip instead
    would resolve a different base with the same subjects.
    """
    main, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "docs(core): describe the thing")
    commit(main, {"b.txt": "b\n"}, "feat(core): a new thing")
    git(main, "push", "-q", "origin", "master")
    git(wt, "fetch", "-q", "origin")
    out = run(["bash", str(ROOT / "scripts/pr-title-range.sh")], wt, {"BASE_REF": "master"})
    assert out.returncode == 0, out.stdout + out.stderr
    base_sha = out.stdout.strip().removesuffix("..HEAD")
    tip = git(wt, "rev-parse", "origin/master").stdout.strip()
    assert base_sha != tip
    assert base_sha == git(wt, "merge-base", "origin/master", "HEAD").stdout.strip()
    subjects = git(wt, "log", "--no-merges", "--format=%s", out.stdout.strip()).stdout.splitlines()
    assert subjects == ["docs(core): describe the thing"]


def test_the_pr_range_script_reports_an_unresolvable_base(tmp_path):
    """A base with no merge base fails loudly instead of printing an empty range."""
    _, wt = fixture(tmp_path)
    out = run(["bash", str(ROOT / "scripts/pr-title-range.sh")], wt, {"BASE_REF": "no-such-branch"})
    assert out.returncode != 0
    assert "no merge base" in out.stderr


def _behind(main: Path, wt: Path, branch_files: dict, master_files: dict) -> None:
    commit(wt, branch_files, "feat(core): a branch thing")
    for name, text in master_files.items():
        commit(main, {name: text}, f"docs(core): master adds {name}")
    git(main, "push", "-q", "origin", "master")
    git(wt, "fetch", "-q", "origin")


def test_a_branch_behind_without_overlap_or_conflict_passes(tmp_path):
    """Only an overlap or a conflict needs a merge first; CI tests the rest."""
    main, wt = fixture(tmp_path)
    _behind(main, wt, {"branch.txt": "b\n"}, {"master.txt": "m\n"})
    result = ritual(wt)
    assert "behind origin/master" not in result.stdout, result.stdout
    assert "merge origin/master" not in result.stdout, result.stdout
    assert "behind" in result.stdout, result.stdout


def test_a_branch_behind_with_overlap_merges_first(tmp_path):
    main, wt = fixture(tmp_path)
    _behind(main, wt, {"shared.txt": "branch\n"}, {"shared.txt": "master\n"})
    out = ritual(wt).stdout
    assert "merge origin/master" in out, out


def test_a_branch_behind_with_conflict_merges_first(tmp_path):
    """A file on one side colliding with a directory on the other conflicts."""
    main, wt = fixture(tmp_path)
    commit(wt, {"clash": "a file\n"}, "feat(core): a branch thing")
    commit(main, {"clash/inner.txt": "a dir\n"}, "docs(core): master adds a directory")
    git(main, "push", "-q", "origin", "master")
    git(wt, "fetch", "-q", "origin")
    out = ritual(wt).stdout
    assert "merge origin/master" in out, out


def test_a_takeoff_pass_makes_the_push_skip_the_suite(tmp_path):
    """The takeoff records the pre-push stamp, so the push runs nothing twice."""
    _, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    head = git(wt, "rev-parse", "HEAD").stdout.strip()
    line = f"refs/heads/feat/1-thing {head} refs/heads/feat/1-thing {'0' * 40}\n"
    suite_log = tmp_path / "suite.log"
    suite = f"echo ran >> {suite_log}"
    body = tmp_path / "body.md"
    body.write_text(GOOD_BODY)
    git(wt, "config", "user.email", "t@example.com")
    result = ritual(wt, TEST="1", PR_BODY=str(body), PR_TITLE="feat(core): a", R2S_SUITE=suite)
    assert result.returncode == 0, result.stdout + result.stderr
    assert suite_log.read_text().split() == ["ran"]
    assert pre_push(wt, line, R2S_PRE_PUSH_SUITE=suite).returncode == 0
    assert suite_log.read_text().split() == ["ran"], "the push reran the suite"


def test_full_env_runs_the_suite_unscoped(tmp_path):
    """R2S_FULL=1 forces the full suite: the hook passes no scope at all."""
    _, wt = fixture(tmp_path)
    commit(wt, {"a.txt": "a\n"}, "feat(core): a")
    head = git(wt, "rev-parse", "HEAD").stdout.strip()
    line = f"refs/heads/feat/1-thing {head} refs/heads/feat/1-thing {'0' * 40}\n"
    seen = tmp_path / "env.txt"
    suite = f"env > {seen}"
    assert pre_push(wt, line, R2S_PRE_PUSH_SUITE=suite, R2S_FULL="1").returncode == 0
    text = seen.read_text()
    assert "FULL=1" not in text
    for var in ("LINT", "SCRIPTS", "COMMITS", "CORE", "GEN", "BRIDGE", "E2E", "COVERAGE", "DOCS", "EXAMPLES", "FUZZ"):
        assert f"{var}=1" not in text, f"scoped {var} in a full run"


def test_ci_runs_every_scope_on_pull_requests():
    """The local gate is scoped; CI still runs the full suite on the merge."""
    text = (ROOT / ".github/workflows/ci.yml").read_text()
    assert "pull_request" in text
    runs = [line.strip() for line in text.splitlines() if "make test" in line]
    assert runs, "CI runs no suite at all"
    assert not any("R2S_FULL" in line for line in runs), "CI must not force a full local run"
    mentioned = set()
    for line in runs:
        for scope in ("LINT", "SCRIPTS", "COMMITS", "CORE", "GEN", "BRIDGE", "COVERAGE", "DOCS", "EXAMPLES", "FUZZ"):
            if scope in line:
                mentioned.add(scope)
        if "E2E=avr" in line:
            mentioned.add("E2E=avr")
        if "E2E=posix" in line:
            mentioned.add("E2E=posix")
    missing = {
        "LINT",
        "SCRIPTS",
        "COMMITS",
        "CORE",
        "GEN",
        "BRIDGE",
        "COVERAGE",
        "DOCS",
        "EXAMPLES",
        "FUZZ",
        "E2E=avr",
        "E2E=posix",
    } - mentioned
    assert not missing, f"CI no longer runs every scope: {sorted(missing)}"
