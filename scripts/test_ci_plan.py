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


def test_the_plan_exposes_one_output_per_job():
    plan = jobs()['plan']
    assert set(plan['outputs']) == set(PLAN_OUTPUTS)


def test_the_plan_runs_the_map_against_the_pull_request_base():
    steps = jobs()['plan']['steps']
    runs = ' '.join(str(step.get('run', '')) for step in steps)
    assert 'tools/ci_jobs.py' in runs
    assert '--base "origin/$BASE_REF"' in runs
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
    for name, job in jobs().items():
        if name in ALWAYS_RUN or 'if' not in job:
            continue
        assert 'github.event_name' in job['if'], name


def test_each_section_job_needs_the_plan_and_gates_on_its_output():
    known = set(ci_jobs.JOBS)
    for name in sorted(known):
        job = jobs()[name]
        assert 'plan' in needs(job), name
        condition = str(job['if'])
        output = ci_jobs.JOBS[name]
        assert 'needs.plan.outputs.full' in condition, name
        assert f'needs.plan.outputs.{output}' in condition, name


def test_the_e2e_legs_split_on_the_shared_backend_value():
    jobs_ = jobs()
    avr = str(jobs_['e2e-avr']['if'])
    posix = str(jobs_['e2e-posix']['if'])
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


def test_the_plan_restores_no_checksum():
    """The checksum plan is gone: no job restores or saves a Task checksum."""
    text = WORKFLOW.read_text(encoding='utf-8')
    assert '.task/checksum' not in text
    assert 'cache/restore' not in text


def test_the_scripts_shards_run_without_their_own_save_job():
    shards = jobs()['scripts']
    assert shards['strategy']['matrix']['shard']
    assert 'scripts-save' not in jobs()


def test_the_bridge_matrix_runs_without_its_own_save_job():
    assert 'bridge-save' not in jobs()
    assert 'bridge-images' in jobs()


def test_the_e2e_posix_matrix_runs_without_its_own_save_job():
    assert 'e2e-posix-save' not in jobs()
