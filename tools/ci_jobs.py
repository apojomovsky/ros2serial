# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Map a pull request's changed files to the CI jobs its files can affect.

The map has three parts, in order of precedence:

- the trees every job builds in (the core, the C++ layer, the ports, the
  bridge, the generator), the image definition, the suite plumbing and the
  workflows select the full matrix, because a change there reaches every
  section;
- a prose-only change (a page under docs/, the README or the changelog)
  selects the jobs that read prose: the drift and snippet gates, the
  mermaid parser and the standing lint gate, never a build;
- anything else selects the section tasks whose `sources:` in Taskfile.yml
  list it, so the map and the Taskfile cannot disagree, and a tools script
  runs its own tests and the gate that uses it.

A path no section reads selects the full matrix, and so does an empty
diff: an unknown path must never select too few, and a plan that selected
nothing would read as green.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TASKFILE = 'Taskfile.yml'

# The plan output that forces every job, whatever the diff says.
FULL = 'full'

# The pull request label that opts into the long fuzzing.
FUZZ_LONG_LABEL = 'fuzz-long'

# The section tasks and the ci.yml job each one runs. A task listed here is
# a planned job; `tools/taskfile_coverage.py` reads this table, so a task
# added to the Taskfile without a job fails the lint gate.
TASK_JOBS = {
    'lint': 'lint',
    'ament_lint': 'ament-lint',
    'scripts': 'scripts',
    'core': 'core',
    'gen': 'gen',
    'cpp': 'cpp',
    'docs': 'docs',
    'mermaid': 'mermaid',
    'readme': 'readme',
    'coverage': 'coverage',
    'bridge': 'bridge',
    'e2e-avr': 'e2e-avr',
    'e2e-posix': 'e2e-posix',
    'fuzz': 'fuzz-smoke',
    'nightly': 'e2e-isolation',
    'examples': 'build-matrix',
    'esp32': 'esp32',
    'pic': 'pic-canary',
    'stm32': 'stm32',
    'rp2040': 'rp2040',
    'platformio': 'platformio',
    'budgets': 'budgets',
}

# Every ci.yml section job and the plan output it gates on. The e2e legs
# share one output and split on its backend value.
JOBS = {
    'lint': 'lint',
    'ament-lint': 'ament_lint',
    'scripts': 'scripts',
    'core': 'core',
    'gen': 'gen',
    'cpp': 'cpp',
    'docs': 'docs',
    'mermaid': 'mermaid',
    'readme': 'readme',
    'coverage': 'coverage',
    'bridge': 'bridge',
    'e2e-avr': 'e2e',
    'e2e-posix': 'e2e',
    'fuzz-smoke': 'fuzz',
    'e2e-isolation': 'nightly',
    'build-matrix': 'examples',
    'esp32': 'esp32',
    'pic-canary': 'pic',
    'stm32': 'stm32',
    'rp2040': 'rp2040',
    'platformio': 'platformio',
    'budgets': 'budgets',
}

# The plan output each task selects.
OUTPUTS = {task: JOBS[job] for task, job in TASK_JOBS.items()}

# Tasks ci.yml never plans, each with the reason it stays out.
TASKS_WITHOUT_A_JOB = {
    'default': 'the aggregate that runs every section locally',
    'local': 'the fast gates the hook and the takeoff run',
    'commits': 'pr-title.yml runs it on every pull request event',
    'soak': 'schedule-only soak, selected by name and by the schedule',
    'bigendian': 'nightly-only big-endian job, selected by name and by the schedule',
    'pic-sim': 'nightly-only MPLAB SIM job, selected by name and by the schedule',
}

# The trees every section builds in, the image, the suite plumbing and the
# workflows: a change here reaches every job, so it runs the full matrix.
SHARED = (
    'Dockerfile',
    '.dockerignore',
    '.gitignore',
    'Makefile',
    'Taskfile.yml',
    'VERSION',
    'conftest.py',
    'pyproject.toml',
    'ci/**',
    '.github/**',
    'scripts/pass-vars.list',
    'scripts/ci-test.sh',
    'scripts/run-tasks.sh',
    'scripts/suite-lock.sh',
    'scripts/clean-git-env.sh',
    'scripts/with-build-lock.sh',
    'scripts/e2e-lease.sh',
    'tools/ci_jobs.py',
    'tools/test_ci_jobs.py',
    'core/**',
    'cpp/**',
    'ports/**',
    'bridge/**',
    'gen/**',
    'rosidl/**',
)

# A prose change: no build can see it, so the gates that read prose run,
# and so does any task that names the page in its sources. Every markdown
# page feeds the mermaid parser, and the README carries the anchors the
# walkthrough check greps for.
PROSE = {
    'docs/**': ('docs', 'mermaid'),
    'README.md': ('docs', 'mermaid', 'readme'),
    'CHANGELOG.md': ('docs', 'mermaid'),
}


def glob_match(pattern, path):
    """Doublestar matching as Task resolves it: `*` stays inside a level."""
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


# A runner invocation: an interpreter, then a repo-relative script path. The
# path may be quoted, rooted or relative, because the runners write it both
# ways (`bash scripts/ci-test.sh`, `bash "$root/scripts/core-test.sh"`).
INVOKE = re.compile(
    r'(?:^|[\s;&|(])(?:exec\s+)?(?:bash|sh|python3|source)\s+'
    r'(?:env\s+\S+\s+)?(?:"|\')?(?:(?:\$(?:root|\{root\})/|\./)|'
    r'(?=(?:scripts|tools|tests|ports|examples|core|gen|bridge|benchmark)/))'
    r'((?:[A-Za-z0-9_.-]+/)+[A-Za-z0-9_.$-]+\.(?:sh|py))'
)

# A scope assignment a task's `cmds` hands the suite (`CORE=1`, `E2E=both`).
SCOPE_ASSIGN = re.compile(r'\b([A-Z0-9_]+)=(\S+)')


def _invoked(text):
    """The repo scripts one text runs, as written."""
    return {match.group(1) for match in INVOKE.finditer(text)}


def _scope_blocks(text):
    """Each `if wants VAR; then ... fi` block of the suite runner, by VAR."""
    blocks = {}
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        found = re.match(r'\s*if wants ([A-Z0-9_]+); then(.*)$', lines[index])
        if not found:
            index += 1
            continue
        scope, tail = found.group(1), found.group(2)
        if tail.rstrip().endswith('fi'):
            body, index = [tail], index + 1
        else:
            body, depth, cursor = [tail], 1, index + 1
            while cursor < len(lines):
                line = lines[cursor]
                body.append(line)
                if re.match(r'\s*if\b', line):
                    depth += 1
                if re.match(r'\s*fi\b', line):
                    depth -= 1
                    if depth == 0:
                        break
                cursor += 1
            index = cursor + 1
        blocks.setdefault(scope, []).append('\n'.join(body))
    return blocks


def _functions(text):
    """The shell functions a runner defines, by name.

    Brace counting, not a regex: a one-line function (`f() { ...; }`) leaves
    its closing brace mid-line, and a lazy regex then swallows the next
    function whole.
    """
    found = {}
    for match in re.finditer(r'(?m)^([a-z_][a-z0-9_]*)\(\)\s*\{', text):
        depth, cursor = 0, match.end() - 1
        while cursor < len(text):
            char = text[cursor]
            if char == '{':
                depth += 1
            elif char == '}':
                depth -= 1
                if depth == 0:
                    break
            cursor += 1
        found[match.group(1)] = text[match.start() : cursor + 1]
    return found


def _case_arms(text, values):
    """Keep only the arms of a `case "${VAR:-}" in` that the value selects.

    The e2e block dispatches on the backend (`E2E=both` runs two runners, a
    single backend runs one), so reading every arm would credit a leg with
    the other leg's scripts.
    """
    selected = None
    for scope, value in values.items():
        if re.search(r'case\s+"?\$\{' + scope + r':-\}"?\s+in\b', text):
            selected = value
            break
    if selected is None:
        return text
    lines = text.splitlines(keepends=True)
    out, depth, take = [], 0, True
    for line in lines:
        label = re.match(r'\s*([^\s()]+)\)', line)
        if label and depth == 1:
            names = {part.strip('"\'') for part in label.group(1).split('|')}
            take = selected in names or '' in names and selected == ''
        if take:
            out.append(line)
        if re.search(r'\bcase\b', line):
            depth += 1
        if re.search(r'\besac\b', line):
            depth = max(0, depth - 1)
    return ''.join(out)


def _body(path, text, scopes, values):
    """The text one invoked script contributes, cut to the task's scopes.

    `scripts/ci-test.sh` is the suite's dispatcher: reading it whole would
    credit every section to every scope. The task's `cmds` name the scope, so
    only that scope's block runs, plus the helper functions it calls.
    """
    if path != 'scripts/ci-test.sh' or not scopes:
        return text
    blocks = _scope_blocks(text)
    functions = _functions(text)
    chosen = [_case_arms(body, values) for scope in scopes for body in blocks.get(scope, [])]
    called = {
        name
        for body in chosen
        for name in re.findall(r'\b([a-z_][a-z0-9_]*)\b', body)
        if name in functions
    }
    pending, extra = list(called), []
    while pending:
        name = pending.pop()
        extra.append(functions[name])
        for inner in re.findall(r'\b([a-z_][a-z0-9_]*)\b', functions[name]):
            if inner in functions and inner not in called:
                called.add(inner)
                pending.append(inner)
    return '\n'.join(chosen) + '\n' + ''.join(extra)


# Tasks Task always runs: with no `sources:`, nothing can drop them from a
# pull request, so a file they reach is never a selection gap.
ALWAYS_RUN = {'lint', 'commits'}


def reachable_scripts(cmds, repo=ROOT):
    """Every `scripts/` and `tools/` file one task's commands reach.

    Follows the runners a task names, then the runners those name, so a
    helper no task lists in its `sources:` shows up as a file that can break
    the section without ever selecting it. Every repo script reached is
    returned, `tests/` runners included, because a test helper that calls an
    unlisted script hides the same gap one step further out.
    """
    root = Path(repo)
    values = dict(SCOPE_ASSIGN.findall(cmds))
    scopes = set(values)
    reached, seen = set(), set()
    pending = [('', cmds)]
    while pending:
        path, text = pending.pop()
        for target in _invoked(text):
            if '$' in target or target in seen:
                continue
            seen.add(target)
            file = root / target
            if not file.is_file():
                continue
            reached.add(target)
            pending.append(
                (target, _body(target, file.read_text(errors='ignore'), scopes, values))
            )
    return reached


def sections(taskfile=ROOT / TASKFILE):
    """The section tasks' source patterns, by task name."""
    import yaml

    tasks = yaml.safe_load(Path(taskfile).read_text(encoding='utf-8'))['tasks']
    return {name: task.get('sources') or [] for name, task in tasks.items() if name in TASK_JOBS}


def covers(sources, path):
    """Whether one task reads this path, honoring its exclude maps."""
    if not any(isinstance(source, str) and glob_match(source, path) for source in sources):
        return False
    return not any(
        isinstance(source, dict) and glob_match(source.get('exclude', ''), path)
        for source in sources
    )


def select(paths, by_task=None):
    """The plan outputs one changed-file list selects."""
    by_task = sections() if by_task is None else by_task
    paths = [path.strip() for path in paths if path and path.strip()]
    if not paths:
        return {FULL: '1'}
    outputs = {'lint': '1'}
    backends = set()
    for path in paths:
        if any(glob_match(pattern, path) for pattern in SHARED):
            return {FULL: '1'}
        hit = False
        # A page under docs/ is prose, but a task may still name it as an
        # input: one of the gates keeps the page honest, so both select.
        prose = next((jobs for pattern, jobs in PROSE.items() if glob_match(pattern, path)), None)
        for job in prose or ():
            outputs[job] = '1'
        for name, sources in by_task.items():
            if not covers(sources, path):
                continue
            hit = True
            if name in ('e2e-avr', 'e2e-posix'):
                backends.add(name.removeprefix('e2e-'))
                continue
            outputs[OUTPUTS[name]] = '1'
        if prose is None and not hit:
            return {FULL: '1'}
    if backends:
        outputs['e2e'] = 'both' if len(backends) == 2 else backends.pop()
    return dict(sorted(outputs.items()))


def jobs_for(outputs):
    """The ci.yml jobs one plan output map runs."""
    if outputs.get(FULL) == '1':
        return set(JOBS)
    backends = outputs.get('e2e', '')
    chosen = set()
    for job, output in JOBS.items():
        if job in ('e2e-avr', 'e2e-posix'):
            if backends in ('both', job.removeprefix('e2e-')):
                chosen.add(job)
        elif output in outputs:
            chosen.add(job)
    return chosen


def _git(root, *args):
    proc = subprocess.run(['git', *args], cwd=root, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f'git {" ".join(args)} failed: {proc.stderr.strip()}')
    return [line for line in proc.stdout.splitlines() if line]


def changed_paths(base, repo=ROOT):
    """Every path the branch touches since its merge base with base.

    `--name-status` reports a rename as its old and new path, so a file
    moved out of a subtree still selects that subtree's jobs.
    """
    root = Path(repo)
    merge_base = _git(root, 'merge-base', base, 'HEAD')[0]
    paths = set()
    for line in _git(root, 'diff', '--name-status', merge_base, 'HEAD'):
        paths.update(line.split('\t')[1:])
    return sorted(paths)


def main(argv=None, repo=ROOT):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', default='origin/master')
    args = parser.parse_args(argv)
    try:
        paths = changed_paths(args.base, repo=repo)
    except (RuntimeError, IndexError) as exc:
        print(f'ci-jobs: {exc}; running the full matrix', file=sys.stderr)
        paths = ['?']
    for key, value in select(paths).items():
        print(f'{key}={value}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
