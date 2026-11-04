"""The nightly and release workflows run what is too long for a PR.

The e2e isolation pairs left the SCRIPTS scope for a reusable workflow
beside the long fuzzing. nightly.yml calls it on schedule and files an
issue when the scheduled run fails; release.yml calls it on a tag before
anything is published. These tests pin that contract from the files.
"""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
CI = ROOT / ".github" / "workflows" / "ci.yml"
JOBS = ROOT / ".github" / "workflows" / "nightly-jobs.yml"
NIGHTLY = ROOT / ".github" / "workflows" / "nightly.yml"
RELEASE = ROOT / ".github" / "workflows" / "release.yml"

FAILURE_TITLE = "Nightly run failed"


def workflow(path):
    assert path.is_file(), f"{path.name} is missing"
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def triggers(doc):
    return doc.get(True, doc.get("on"))


def steps_of(doc, name):
    return doc["jobs"][name]["steps"]


def test_the_nightly_jobs_are_reusable_and_trigger_on_nothing():
    assert set(triggers(workflow(JOBS))) == {"workflow_call"}


def test_the_long_fuzzing_runs_unless_the_caller_turns_it_off():
    doc = workflow(JOBS)
    fuzz = triggers(doc)["workflow_call"]["inputs"]["fuzz"]
    assert (fuzz["type"], fuzz["default"]) == ("boolean", True)
    assert doc["jobs"]["fuzz-long"].get("if") == "${{ inputs.fuzz }}"


def test_the_scheduled_nightly_keeps_the_long_fuzzing():
    for job in workflow(NIGHTLY)["jobs"].values():
        if "nightly-jobs.yml" in str(job.get("uses", "")):
            assert (job.get("with") or {}).get("fuzz", True) is True


def test_the_reusable_workflow_holds_fuzzing_isolation_and_soak():
    assert set(workflow(JOBS)["jobs"]) == {"fuzz-long", "e2e-isolation", "soak"}


def test_the_soak_runs_only_when_the_caller_asks_for_it():
    doc = workflow(JOBS)
    soak = triggers(doc)["workflow_call"]["inputs"]["soak"]
    assert (soak["type"], soak["default"]) == ("boolean", False)
    assert doc["jobs"]["soak"].get("if") == "${{ inputs.soak }}"


def test_the_scheduled_nightly_runs_the_soak():
    for job in workflow(NIGHTLY)["jobs"].values():
        if "nightly-jobs.yml" in str(job.get("uses", "")):
            assert (job.get("with") or {}).get("soak", False) is True


def test_pr_ci_never_runs_the_soak():
    job = ci_nightly_call()
    assert (job.get("with") or {}).get("soak") is False


def ci_nightly_call():
    calls = {name: job for name, job in workflow(CI)["jobs"].items() if "nightly-jobs.yml" in str(job.get("uses", ""))}
    assert len(calls) == 1, f"ci.yml should call nightly-jobs.yml once: {sorted(calls)}"
    return next(iter(calls.values()))


def test_pr_ci_runs_the_reusable_nightly_jobs_when_the_plan_selects_nightly():
    job = ci_nightly_call()
    needs = job["needs"] if isinstance(job["needs"], list) else [job["needs"]]
    assert "plan" in needs
    condition = str(job.get("if", ""))
    assert "github.event_name" in condition
    assert "needs.plan.outputs.full" in condition
    assert "needs.plan.outputs.nightly" in condition


def test_pr_ci_leaves_out_the_long_fuzzing_unless_fuzz_is_selected():
    job = ci_nightly_call()
    assert "timeout-minutes" not in job
    fuzz = (job.get("with") or {}).get("fuzz")
    assert str(fuzz).strip() == "${{ needs.plan.outputs.fuzz == '1' }}"


def test_the_release_call_skips_the_long_fuzzing():
    calls = [job for job in workflow(RELEASE)["jobs"].values() if "nightly-jobs.yml" in str(job.get("uses", ""))]
    assert len(calls) == 1
    assert (calls[0].get("with") or {}).get("fuzz") is False


def test_fuzz_long_keeps_its_half_hour_per_target():
    steps = steps_of(workflow(JOBS), "fuzz-long")
    assert any("make test FUZZ=1" in str(step.get("run", "")) for step in steps)
    envs = [step.get("env", {}) or {} for step in steps]
    assert any(str(env.get("R2S_FUZZ_SECONDS")) == "1800" for env in envs)


def test_the_isolation_job_runs_the_nightly_scope_with_both_envs():
    steps = steps_of(workflow(JOBS), "e2e-isolation")
    assert any("./.github/actions/dev-image" in str(step.get("uses", "")) for step in steps)
    runs = [step for step in steps if "make test NIGHTLY=1" in str(step.get("run", ""))]
    assert len(runs) == 1
    env = runs[0].get("env", {})
    assert str(env.get("R2S_E2E_SKIP")) == "test_scenario_8_posix"
    assert str(env.get("R2S_SYNC_BOUND_NS")) == "5000000"


def pass_vars():
    """The variables make forwards into the dev image, one per list line."""
    return (ROOT / "scripts" / "pass-vars.list").read_text().split()


def test_make_forwards_the_nightly_scope_and_its_env():
    for var in ("NIGHTLY", "R2S_E2E_SKIP", "R2S_SYNC_BOUND_NS"):
        assert var in pass_vars(), f"make drops {var} before the container"


def test_the_soak_job_runs_the_soak_scope_for_two_hours():
    steps = steps_of(workflow(JOBS), "soak")
    assert any("./.github/actions/dev-image" in str(step.get("uses", "")) for step in steps)
    runs = [step for step in steps if "make test SOAK=1" in str(step.get("run", ""))]
    assert len(runs) == 1
    assert str(runs[0].get("env", {}).get("R2S_SOAK_SECONDS")) == "7200"


def test_make_forwards_the_soak_scope_and_its_env():
    for var in ("SOAK", "R2S_SOAK_SECONDS", "R2S_SOAK_BACKENDS"):
        assert var in pass_vars(), f"make drops {var} before the container"


def test_nightly_calls_the_reusable_workflow_on_schedule():
    doc = workflow(NIGHTLY)
    on = triggers(doc)
    assert on["schedule"] == [{"cron": "0 3 * * *"}]
    assert "workflow_dispatch" in on
    assert any("nightly-jobs.yml" in str(job.get("uses", "")) for job in doc["jobs"].values())


def test_a_failed_scheduled_nightly_files_or_comments_one_issue():
    doc = workflow(NIGHTLY)
    reporters = [
        job
        for job in doc["jobs"].values()
        if "gh issue create" in "\n".join(str(step.get("run", "")) for step in job.get("steps", []))
    ]
    assert len(reporters) == 1
    reporter = reporters[0]
    assert reporter["permissions"].get("issues") == "write"
    condition = str(reporter.get("if", ""))
    assert "failure()" in condition and "schedule" in condition
    needs = reporter.get("needs", [])
    if isinstance(needs, str):
        needs = [needs]
    assert any("nightly-jobs.yml" in str(doc["jobs"][need].get("uses", "")) for need in needs)
    runs = "\n".join(str(step.get("run", "")) for step in reporter["steps"])
    assert FAILURE_TITLE in runs
    assert "needs-triage" in runs
    assert "gh issue comment" in runs
    assert "GITHUB_RUN_ID" in runs
    envs = [step.get("env", {}) or {} for step in reporter["steps"]]
    assert any(str(env.get("GH_REPO")) == "${{ github.repository }}" for env in envs)
