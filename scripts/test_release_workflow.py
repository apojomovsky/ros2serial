"""The release workflow: a maintainer's tag builds the artifacts into a GitHub release.

A plain version tag becomes a full release marked Latest, while a tag with a
SemVer pre-release suffix (as in v0.2.0-rc.1) becomes a pre-release. The
release attaches the Arduino library zip and the Python sdist and wheel and
publishes nowhere else. This test pins that contract: the tag trigger, the
dry-run path without publishing, the GITHUB_TOKEN scoped to the release job,
and no personal token or PyPI step.
"""

import subprocess
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
FLAGS_SCRIPT = ROOT / "scripts" / "release-flags.sh"


def release_flag(tag):
    out = subprocess.run(["bash", str(FLAGS_SCRIPT), tag], capture_output=True, text=True, check=True)
    return out.stdout.strip()


def workflow():
    assert WORKFLOW.is_file(), "release.yml is missing"
    return yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))


def steps_of(doc):
    (job,) = [job for name, job in doc["jobs"].items() if name == "release"]
    return job


def test_the_workflow_runs_on_a_version_tag_and_dry_runs_without_publishing():
    doc = workflow()
    on = doc.get(True, doc.get("on"))
    assert on["push"]["tags"] == ["v*"]
    assert on["workflow_dispatch"]["inputs"]["dry_run"]["default"] is True
    assert ".github/workflows/release.yml" in on["pull_request"]["paths"]


def test_only_the_release_job_can_write():
    doc = workflow()
    top = doc.get("permissions", {})
    assert top.get("contents") != "write", top
    writers = [name for name, job in doc["jobs"].items() if job.get("permissions", {}).get("contents") == "write"]
    assert writers == ["release"], writers


def test_the_release_uses_the_built_in_token_and_publishes_nowhere_else():
    text = WORKFLOW.read_text(encoding="utf-8")
    assert "secrets." not in text
    assert "github.token" in text
    runs = "\n".join(str(step.get("run", "")) for step in steps_of(workflow())["steps"])
    lowered = runs.lower()
    assert "pypi" not in lowered
    assert "twine" not in lowered


def test_the_tag_builds_the_shared_artifact_script_and_reads_the_changelog():
    job = steps_of(workflow())
    runs = "\n".join(str(step.get("run", "")) for step in job["steps"])
    assert "scripts/build-release-artifacts.sh" in runs
    assert "CHANGELOG.md" in runs


def test_a_plain_tag_publishes_as_a_full_release_marked_latest():
    assert release_flag("v0.2.0") == "--latest"


def test_a_suffixed_tag_publishes_as_a_pre_release():
    assert release_flag("v0.2.0-rc.1") == "--prerelease"


def test_only_a_release_tag_without_dry_run_creates_the_release():
    job = steps_of(workflow())
    publishers = [step for step in job["steps"] if "gh release create" in str(step.get("run", ""))]
    assert len(publishers) == 1
    run = str(publishers[0]["run"])
    assert "release-flags.sh" in run, run
    assert "--prerelease" not in run and "--latest" not in run, run
    guard = str(publishers[0].get("if", ""))
    assert "refs/tags/v" in guard and "dry_run" in guard, guard


def test_release_calls_the_nightly_jobs_on_a_tag_only():
    doc = workflow()
    callers = [name for name, job in doc["jobs"].items() if "nightly-jobs.yml" in str(job.get("uses", ""))]
    assert len(callers) == 1
    assert "refs/tags/v" in str(doc["jobs"][callers[0]].get("if", ""))


def test_release_skips_the_time_boxed_fuzzing():
    """Fuzzing for a fixed time can disagree between two runs of one commit;
    the seed corpus replay in CI is the deterministic part."""
    doc = workflow()
    (caller,) = [job for job in doc["jobs"].values() if "nightly-jobs.yml" in str(job.get("uses", ""))]
    assert (caller.get("with") or {}).get("fuzz") is False


def test_release_publishes_only_after_the_nightly_jobs():
    doc = workflow()
    callers = [name for name, job in doc["jobs"].items() if "nightly-jobs.yml" in str(job.get("uses", ""))]
    (job,) = [job for name, job in doc["jobs"].items() if name == "release"]
    needs = job.get("needs", [])
    if isinstance(needs, str):
        needs = [needs]
    assert callers[0] in needs


def test_release_still_builds_when_the_nightly_call_is_skipped():
    """A skipped caller must not skip the build: pull requests touching this
    file and manual dispatches run the same build without publishing."""
    doc = workflow()
    (job,) = [job for name, job in doc["jobs"].items() if name == "release"]
    condition = str(job.get("if", ""))
    assert "always()" in condition, "a skipped nightly call skips the release build"
    assert "needs.nightly.result" in condition
    assert "'success'" in condition and "'skipped'" in condition
    assert "'failure'" not in condition and "'cancelled'" not in condition


def test_a_tag_the_repository_version_does_not_name_refuses_to_publish():
    job = steps_of(workflow())
    gates = [step for step in job["steps"] if "GITHUB_REF_NAME" in str(step.get("run", ""))]
    assert gates, "no step refuses a tag VERSION does not name"
    assert any("refs/tags/v" in str(step.get("if", "")) for step in gates)
    runs = "\n".join(str(step.get("run", "")) for step in gates)
    assert "VERSION" in runs
