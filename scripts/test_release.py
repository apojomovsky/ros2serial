"""The release cut: VERSION is the single source, and the next version follows the commits."""

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True)


def repo_with(tmp_path: Path, tag: str, subjects: list[str]) -> Path:
    repo = tmp_path / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "master")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "user.name", "t")
    version = tag.removeprefix("v")
    (repo / "VERSION").write_text(version + "\n")
    (repo / "gen" / "r2s_gen").mkdir(parents=True)
    (repo / "bridge" / "r2s_bridge").mkdir(parents=True)
    (repo / "gen" / "r2s_gen" / "version.py").write_text(f'__version__ = "{version}"\nPROTOCOL_VERSION = 1\n')
    (repo / "gen" / "package.xml").write_text(
        f'<package format="3">\n  <name>r2s_gen</name>\n  <version>{version}</version>\n</package>\n'
    )
    (repo / "bridge" / "r2s_bridge" / "package.xml").write_text(
        f'<package format="3">\n  <name>r2s_bridge</name>\n  <version>{version}</version>\n</package>\n'
    )
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "chore: start")
    git(repo, "tag", tag)
    for i, s in enumerate(subjects):
        (repo / f"f{i}").write_text(s)
        git(repo, "add", ".")
        git(repo, "commit", "-q", "-m", s)
    return repo


def release(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["bash", str(ROOT / "scripts/release.sh"), *args], cwd=repo, capture_output=True, text=True)


def next_version(repo: Path, *args: str) -> str:
    out = release(repo, "--dry-run", *args)
    assert out.returncode == 0, out.stdout + out.stderr
    m = re.search(r"next version: (\S+)", out.stdout)
    assert m, out.stdout
    return m.group(1)


def test_version_file_is_semver():
    assert re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\n", (ROOT / "VERSION").read_text())


def test_fix_bumps_patch(tmp_path):
    assert next_version(repo_with(tmp_path, "v0.3.1", ["fix(core): a"])) == "0.3.2"


def test_feature_bumps_minor(tmp_path):
    assert next_version(repo_with(tmp_path, "v0.3.1", ["fix(core): a", "feat(core): b"])) == "0.4.0"


def test_breaking_bumps_minor_before_one_point_zero(tmp_path):
    assert next_version(repo_with(tmp_path, "v0.3.1", ["feat(spec)!: c"])) == "0.4.0"


def test_breaking_bumps_major_after_one_point_zero(tmp_path):
    assert next_version(repo_with(tmp_path, "v1.2.0", ["feat(spec)!: c"])) == "2.0.0"


def test_dry_run_changes_nothing(tmp_path):
    repo = repo_with(tmp_path, "v0.3.1", ["feat(core): b"])
    next_version(repo)
    assert (repo / "VERSION").read_text() == "0.3.1\n"
    assert not (repo / "CHANGELOG.md").exists()


def untagged_repo(tmp_path: Path, subjects: list[str]) -> Path:
    repo = repo_with(tmp_path, "v0.0.0", subjects)
    git(repo, "tag", "-d", "v0.0.0")
    return repo


def test_first_release_is_0_1_0(tmp_path):
    assert next_version(untagged_repo(tmp_path, ["feat(core): a"])) == "0.1.0"


def test_fix_with_bang_is_breaking(tmp_path):
    assert next_version(repo_with(tmp_path, "v1.2.0", ["fix(core)!: a"])) == "2.0.0"


def test_manual_bump_overrides_the_commits(tmp_path):
    repo = repo_with(tmp_path, "v0.3.1", ["feat(core): b"])
    assert next_version(repo, "--bump", "patch") == "0.3.2"
    assert next_version(repo, "--bump", "major") == "1.0.0"


def test_nothing_to_release_fails_loudly(tmp_path):
    result = release(repo_with(tmp_path, "v0.3.1", []), "--dry-run")
    assert result.returncode == 1 and "nothing to release" in result.stdout


def test_chore_only_is_nothing_to_release(tmp_path):
    result = release(repo_with(tmp_path, "v0.3.1", ["chore(ci): tidy"]), "--dry-run")
    assert result.returncode == 1 and "nothing to release" in result.stdout


def subject_ok(version: str, subject: str) -> bool:
    """What the tag job's check says about a subject."""
    result = subprocess.run(
        ["bash", str(ROOT / "scripts/release-subject.sh"), version, subject],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def test_bad_arguments_are_reported(tmp_path):
    repo = repo_with(tmp_path, "v0.3.1", ["feat(core): b"])
    for args in (["--bump", "bogus"], ["--bump"], ["--what"]):
        result = release(repo, *args)
        assert result.returncode == 2 and "usage" in result.stdout, (args, result.stdout + result.stderr)


def test_a_real_run_writes_version_and_changelog_in_one_commit(tmp_path):
    repo = repo_with(tmp_path, "v0.3.1", ["feat(core): b"])
    (repo / "sub").mkdir()
    result = subprocess.run(
        ["bash", str(ROOT / "scripts/release.sh")], cwd=repo / "sub", capture_output=True, text=True
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert (repo / "VERSION").read_text() == "0.4.0\n"
    assert "B" in (repo / "CHANGELOG.md").read_text()
    log = subprocess.run(["git", "log", "-1", "--format=%s"], cwd=repo, capture_output=True, text=True).stdout.strip()
    assert log == "chore(release): v0.4.0"
    assert "next version: 0.4.0" in result.stdout


def test_a_real_run_writes_the_version_into_the_generator_package(tmp_path):
    repo = repo_with(tmp_path, "v0.3.1", ["feat(core): b"])
    result = subprocess.run(["bash", str(ROOT / "scripts/release.sh")], cwd=repo, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert '__version__ = "0.4.0"' in (repo / "gen" / "r2s_gen" / "version.py").read_text()
    assert "<version>0.4.0</version>" in (repo / "gen" / "package.xml").read_text()
    assert "<version>0.4.0</version>" in (repo / "bridge" / "r2s_bridge" / "package.xml").read_text()


def test_release_commits_stay_out_of_the_changelog(tmp_path):
    repo = repo_with(tmp_path, "v0.3.1", ["chore(release): v0.3.1", "feat(core): b"])
    out = release(repo, "--dry-run").stdout
    assert "chore(release)" not in out and "V0.3.1" not in out


def test_the_release_subject_is_recognised_after_a_squash(tmp_path):
    """A squash puts the PR number after the subject, so it is matched by prefix.

    The tag job reads the subject of the commit that last changed VERSION and
    refuses to tag unless it is the release commit. The check is a script, so
    the job and this test cannot drift apart.
    """
    repo = repo_with(tmp_path, "v0.3.1", ["feat(core): b"])
    (repo / "VERSION").write_text("0.4.0\n")
    git(repo, "add", "VERSION")
    git(repo, "commit", "-q", "-m", "chore(release): v0.4.0 (#99)")
    version = (repo / "VERSION").read_text().strip()
    subject = subprocess.run(
        ["git", "log", "-1", "--format=%s", "--", "VERSION"],
        cwd=repo,
        capture_output=True,
        text=True,
    ).stdout.strip()
    assert subject == "chore(release): v0.4.0 (#99)"
    assert subject_ok(version, subject), subject


def test_a_subject_that_is_not_a_release_commit_is_refused(tmp_path):
    """The check is a prefix match, so a later commit is not a release."""
    assert subject_ok("0.4.0", "chore(release): v0.4.0 (#99)")
    assert not subject_ok("0.4.0", "chore(release): v0.3.1 (#99)")
    assert not subject_ok("0.4.0", "ci: add the ticket selector")
    assert not subject_ok("0.4.0", "chore(release): v0.4.0x")
