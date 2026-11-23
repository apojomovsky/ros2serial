# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""ci.yml runs the jobs a pull request's files can affect.

The plan job maps the diff through tools/ci_jobs.py and exposes one output
per ci.yml job. Every section job needs the plan and gates on its output,
so a skipped job still reports and branch protection needs no change.
Pushes to master and manual runs keep the full matrix, and so does a FULL
result.
"""

import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))

import ci_jobs

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / '.github' / 'workflows' / 'ci.yml'

# The jobs that only set up: they run on every event.
ALWAYS_RUN = ('plan', 'bridge-images')

# The jobs whose needs make them skip with a section.
SECTION_SAVE = ()

PLAN_OUTPUTS = (
    'lint',
    'ament_lint',
    'scripts',
    'core',
    'gen',
    'cpp',
    'docs',
    'mermaid',
    'readme',
    'coverage',
    'bridge',
    'e2e',
    'fuzz',
    'nightly',
    'examples',
    'esp32',
    'pic',
    'platformio',
    'stm32',
    'rp2040',
    'budgets',
    'full',
)


def workflow():
    return yaml.safe_load(WORKFLOW.read_text(encoding='utf-8'))


def jobs():
    return workflow()['jobs']


def needs(job):
    found = job.get('needs', [])
    return found if isinstance(found, list) else [found]


# The matrix jobs, whose expanded names the ruleset requires (`bridge (jazzy)`,
# `scripts (1)`, `e2e-posix (jazzy, rmw_cyclonedds_cpp)`). A job-level skip on
# one of them expands no matrix, so GitHub reports the unexpanded name instead.
MATRIX_JOBS = ('scripts', 'bridge', 'e2e-posix')

# A docs-only change: the prose gates select, no matrix job does.
DOCS_ONLY = ('docs/00-design.md', 'README.md', 'CHANGELOG.md')


# What every step of a gated matrix job carries: the plan output reaches the
# steps through the job's R2S_SELECTED, which the job's env sets once.
GATED_STEP = "env.R2S_SELECTED == '1'"


def matrix_jobs():
    return {name: job for name, job in jobs().items() if 'matrix' in job.get('strategy', {})}


def gate(name):
    """The condition that decides one section job runs.

    A plain job carries it as its `if`. A matrix job carries it once as the
    job's R2S_SELECTED environment value, and its steps read that value: a
    job-level skip never expands the matrix, so the required expanded names
    would never report (#702).
    """
    job = jobs()[name]
    if 'if' in job:
        return str(job['if'])
    return str(job['env']['R2S_SELECTED'])


def test_the_plan_exposes_one_output_per_job():
    plan = jobs()['plan']
    assert set(plan['outputs']) == set(PLAN_OUTPUTS)


def test_the_plan_runs_the_map_against_the_pull_request_base():
    steps = jobs()['plan']['steps']
    runs = ' '.join(str(step.get('run', '')) for step in steps)
    assert 'tools/ci_jobs.py' in runs
    assert 'base="origin/$BASE_REF"' in runs
    assert 'python3 tools/ci_jobs.py --base "$base"' in runs
    envs = [step.get('env', {}) or {} for step in steps]
    assert any(
        'github.event.pull_request.base.ref' in str(value)
        for env in envs
        for value in env.values()
    )


def test_the_plan_never_runs_a_section_on_a_pull_request():
    """The plan reads files; it builds nothing."""
    steps = jobs()['plan']['steps']
    runs = ' '.join(str(step.get('run', '')) for step in steps)
    assert 'dev-image' not in runs
    assert 'make test' not in runs


def test_off_a_pull_request_every_section_job_runs():
    for name in jobs():
        if name in ALWAYS_RUN:
            continue
        assert 'github.event_name' in gate(name), name


def test_each_section_job_needs_the_plan_and_gates_on_its_output():
    known = set(ci_jobs.JOBS)
    for name in sorted(known):
        job = jobs()[name]
        assert 'plan' in needs(job), name
        condition = gate(name)
        output = ci_jobs.JOBS[name]
        assert 'needs.plan.outputs.full' in condition, name
        assert f'needs.plan.outputs.{output}' in condition, name


def test_a_matrix_job_carries_its_gate_on_the_steps_not_the_job():
    """A skipped matrix job reports its unexpanded name, never its legs.

    The ruleset requires every expanded name, so a job-level `if` on a matrix
    job leaves those checks expected until the job runs: #702. The gate lives
    on the steps instead, so the matrix expands and each leg reports.
    """
    for name, job in matrix_jobs().items():
        assert 'if' not in job, f'{name} gates the job, so its legs never report'
        assert name in MATRIX_JOBS, f'{name} is a new matrix job; add it to MATRIX_JOBS'
        selected = job.get('env', {}).get('R2S_SELECTED')
        assert selected, f'{name} has no R2S_SELECTED gate for its steps'
        ungated = [
            str(step.get('uses') or step.get('run'))[:40]
            for step in job['steps']
            if step.get('if') != GATED_STEP
        ]
        assert not ungated, f'{name} runs steps outside its gate: {ungated}'


def test_a_docs_only_change_selects_no_matrix_job_and_still_gates_it():
    """The #700 case: no matrix job runs, and each one still reports.

    The plan leaves them out, so their gates read 'skip'; the job is created
    anyway and reports its expanded names, which is what the ruleset needs.
    """
    outputs = ci_jobs.select(list(DOCS_ONLY))
    assert ci_jobs.FULL not in outputs
    chosen = ci_jobs.jobs_for(outputs)
    assert not chosen & set(MATRIX_JOBS), chosen & set(MATRIX_JOBS)
    for name in MATRIX_JOBS:
        condition = gate(name)
        assert f'needs.plan.outputs.{ci_jobs.JOBS[name]}' in condition, name
        assert 'github.event_name' in condition, name


def test_the_e2e_legs_split_on_the_shared_backend_value():
    avr = gate('e2e-avr')
    posix = gate('e2e-posix')
    assert "needs.plan.outputs.e2e == 'avr'" in avr
    assert "needs.plan.outputs.e2e == 'both'" in avr
    assert "needs.plan.outputs.e2e == 'posix'" in posix
    assert "needs.plan.outputs.e2e == 'both'" in posix


def test_a_gated_job_never_needs_a_skippable_job():
    for name, job in jobs().items():
        for need in needs(job):
            if need in ALWAYS_RUN:
                continue
            # A save job may depend on a leg that can skip: no run, no save.
            if name in SECTION_SAVE:
                continue
            assert need in ALWAYS_RUN, f'{name} needs the skippable {need}'


def test_e2e_posix_keeps_its_bridge_images_setup():
    assert set(needs(jobs()['e2e-posix'])) == {'plan', 'bridge-images'}


def test_the_nightly_caller_runs_all_of_master_and_dispatch_and_keeps_the_label():
    condition = str(jobs()['nightly']['if'])
    assert 'github.event_name' in condition
    assert 'needs.plan.outputs.full' in condition
    assert f"contains(github.event.pull_request.labels.*.name, '{ci_jobs.FUZZ_LONG_LABEL}')" in (
        condition
    )


def test_manual_runs_force_every_section():
    assert workflow()['env']['R2S_FULL'] == (
        "${{ github.event_name == 'workflow_dispatch' && '1' || '' }}"
    )


def test_pull_request_runs_supersede_but_master_runs_never_cancel():
    assert 'github.ref' in str(workflow()['concurrency']['group'])
    assert workflow()['concurrency']['cancel-in-progress'] == (
        "${{ github.event_name == 'pull_request' }}"
    )


def test_the_section_jobs_run_through_make_or_the_host_gate():
    for name in ci_jobs.JOBS:
        steps = jobs()[name]['steps']
        runs = ' '.join(str(step.get('run', '')) for step in steps)
        uses = ' '.join(str(step.get('uses', '')) for step in steps)
        assert 'make test' in runs or 'lint-docs.sh' in runs or 'nightly-jobs.yml' in uses, name


def test_the_workflow_file_is_readable_after_the_map_change():
    """No plan output can collide with a GitHub expression operator."""
    for output in PLAN_OUTPUTS:
        assert '-' not in output, output


def test_every_job_carries_a_timeout():
    for name, job in jobs().items():
        if 'uses' in job:
            continue
        assert 'timeout-minutes' in job, name


def test_the_bridge_matrix_keeps_its_images_setup_job():
    """The bridge legs run in the distro images the setup job resolves."""
    assert 'bridge-images' in jobs()
