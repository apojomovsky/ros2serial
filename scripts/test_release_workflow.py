"""The release workflow: a maintainer's tag builds the artifacts into a pre-release.

The 0.1.0 pre-release attaches the Arduino library zip and the Python sdist
and wheel to a GitHub pre-release and publishes nowhere else. This test pins
that contract: the tag trigger, the dry-run path without publishing, the
GITHUB_TOKEN scoped to the release job, and no personal token or PyPI step.
"""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"


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


def test_the_tag_builds_the_shared_artifact_script_and_creates_a_pre_release():
    job = steps_of(workflow())
    runs = "\n".join(str(step.get("run", "")) for step in job["steps"])
    assert "scripts/build-release-artifacts.sh" in runs
    assert "gh release create" in runs and "--prerelease" in runs
    assert "VERSION" in runs
    assert "CHANGELOG.md" in runs
