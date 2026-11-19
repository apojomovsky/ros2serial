# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Every tracked file maps to a job or says why not.

The Taskfile lists what each section reads and `tools/ci_jobs.py` maps
those tasks to the ci.yml jobs they run, so this check fails on a tracked
file no task lists and on a task no job runs. A file no section reads
carries its reason in UNLISTED instead, so adding a file the suite should
cover means listing it in the Taskfile, and wiring a new reader means the
same. A task ci.yml never plans carries its reason in the map's
TASKS_WITHOUT_A_JOB instead.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Tracked files no section reads, each with the reason nothing lists it.
UNLISTED = {
    # Tag, release-cut and scheduled flows run ungated, and the PR-body and
    # PR-title flows run on every pull request event without a plan.
    '.github/workflows/release.yml': 'tag-only flow, runs ungated',
    '.github/workflows/cut-release.yml': 'manual release cut, runs ungated',
    '.github/workflows/pr-body.yml': 'runs on every PR event, ungated',
    '.github/workflows/pr-title.yml': 'runs on every PR event, ungated',
    '.github/workflows/nightly.yml': 'schedule and dispatch only, runs ungated',
    # The image build plumbing: the image content comes from the Dockerfile
    # and ci/versions.env, which every task lists.
    '.github/actions/dev-image/action.yml': (
        'image build plumbing, content is Dockerfile plus versions.env'
    ),
    # A port no suite section builds yet. Wiring one into a section lists
    # it in that section's task.
    'ports/avr': 'no suite section builds this port yet',
    # Read by the standing lint gate, which always runs, so no task lists them.
    'tools/vendor/ament_code_style.cfg': 'read by the standing lint gate, which always runs',
    'LICENSE': 'content pinned by the standing lint gate, which always runs',
    # Read by git at checkout; no section parses it.
    '.gitattributes': 'read by git at checkout, no section parses it',
    # Nightly-only mutation testing runs outside Task, in the mutation job
    # of .github/workflows/nightly-jobs.yml, so no task lists its inputs.
    'docker/mutation': 'nightly-only image, built by the mutation job',
    'mull.yml': 'nightly-only Mull config, read by run-mutation.sh',
    'mutation': 'nightly-only triage inputs and bridge harness, read by run-mutation.sh',
}

# Tasks the plan loop never checks, each with the reason it stays out.
#: Tasks that only group other tasks: they read no sources and CI never plans them.
AGGREGATES = {'default', 'local'}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ci_jobs  # noqa: E402


def glob_match(pattern, path):
    out = []
    i = 0
    while i < len(pattern):
        char = pattern[i]
        if char == '*':
            if pattern[i : i + 3] == '**/':
                out.append('(.*/)?')
                i += 3
            elif pattern[i : i + 2] == '**':
                out.append('.*')
                i += 2
            else:
                out.append('[^/]*')
                i += 1
        elif char == '?':
            out.append('[^/]')
            i += 1
        else:
            out.append(re.escape(char))
            i += 1
    return re.fullmatch(''.join(out), path) is not None


def task_names(taskfile):
    """Every task the Taskfile defines, aggregates included."""
    import yaml

    return list(yaml.safe_load((taskfile).read_text())['tasks'])


def task_sources(taskfile):
    import yaml

    tasks = yaml.safe_load((taskfile).read_text())['tasks']
    by_task = {}
    for name, task in tasks.items():
        if name in AGGREGATES:
            continue
        by_task[name] = task.get('sources', []) or []
    return by_task


def task_covers(sources, path):
    hit = any(isinstance(s, str) and glob_match(s, path) for s in sources)
    if not hit:
        return False
    return not any(isinstance(s, dict) and glob_match(s.get('exclude', ''), path) for s in sources)


def covered(by_task, path):
    return any(task_covers(sources, path) for sources in by_task.values())


def check_plan(task_names):
    """Every task but the aggregates runs a job, and the exceptions are real tasks."""
    planned = ci_jobs.TASK_JOBS
    missing = [
        name
        for name in task_names
        if name not in AGGREGATES
        and name not in planned
        and name not in ci_jobs.TASKS_WITHOUT_A_JOB
    ]
    stale = [name for name in ci_jobs.TASKS_WITHOUT_A_JOB if name not in task_names]
    return missing, stale


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default=str(ROOT))
    args = parser.parse_args(argv)
    root = Path(args.root)
    proc = subprocess.run(
        ['git', 'ls-files'],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        print(f'taskfile-coverage: git ls-files failed: {proc.stderr.strip()}')
        return 1
    by_task = task_sources(root / 'Taskfile.yml')
    unplanned, stale = check_plan(task_names(root / 'Taskfile.yml'))
    if unplanned:
        for name in unplanned:
            print(f'taskfile-coverage: task {name} runs no ci.yml job')
        return 1
    if stale:
        for name in stale:
            print(f'taskfile-coverage: exception {name} is no task')
        return 1
    missing = []
    for path in proc.stdout.splitlines():
        if not path or covered(by_task, path):
            continue
        if any(path == listed or path.startswith(listed.rstrip('/') + '/') for listed in UNLISTED):
            continue
        missing.append(path)
    if missing:
        for path in missing:
            print(f'taskfile-coverage: no task reads {path}')
        return 1
    print('taskfile-coverage: every tracked file maps to a task')
    return 0


if __name__ == '__main__':
    sys.exit(main())
