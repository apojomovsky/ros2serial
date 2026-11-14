# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The documentation names the commands, macros, paths, jobs and scenarios the tree has.

Every check reads the tree, never a list kept here, so a renamed target, a
dropped macro, a new CI job or a new scenario fails the gate until the prose
follows. Exemptions live in the tuples below with the reason they apply.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
CI = Path('.github') / 'workflows' / 'ci.yml'
SCHEMA = Path('gen') / 'r2s_gen' / 'schema' / 'r2s.schema.json'

USER_DOCS = (
    'README.md',
    'CONTRIBUTING.md',
    'AGENTS.md',
    'examples/README.md',
    'docs/00-design.md',
    'docs/01-testing.md',
    'docs/02-architecture.md',
    'docs/03-port-rosserial-sketch.md',
    'docs/04-porting.md',
    'docs/benchmarks/ci-per-job-install-cost.md',
    'docs/benchmarks/encoding-packed-vs-cdr-vs-mavlink.md',
    'docs/benchmarks/framing-crc.md',
    'docs/budgets.md',
    'docs/custom-interfaces.md',
    'docs/firmware.md',
    'docs/installation.md',
    'docs/research/R15-codebase-organization.md',
    'docs/robot-integration.md',
    'docs/robustness.md',
    'docs/services-and-parameters.md',
    'docs/troubleshooting.md',
    'docs/without-hardware.md',
)

# Tokens that look like a macro or a target but stand for a family or a
# placeholder, so no single definition can exist.
MACRO_PLACEHOLDERS = {
    'R2S_ERR_': 'the error-return family, named as a prefix',
    'R2S_LOG_': 'the log-level family, named as a prefix',
    'R2S_WITH_': 'the feature-flag family, named as a prefix',
    'R2S_MYBOARD_HOST_TEST': "the porting guide's invented board name",
}
# `make <word>` in prose is English, not a target: the check reads only the
# fenced blocks and backticked spans, where a command is meant.
MAKE_TOKEN = re.compile(r'(?<![\w`/])make ([a-z][a-z0-9-]*)')

# Jobs with no local twin, each with the reason CONTRIBUTING gives. The
# `-save` jobs share one sentence that names them by suffix.
TWINLESS_JOBS = {
    'plan': 'reports which tasks are stale; make check selects the same scope',
    'bridge-images': 'resolves the digest pins ci/versions.env carries',
    'e2e-isolation': 'runs the nightly isolation pairs, which need a quiet host',
    'nightly': 'runs the shared nightly jobs on their own schedule',
}
SAVE_SUFFIX = '-save'


def _read(rel: str) -> str:
    """One tracked text file, as text."""
    return (ROOT / rel).read_text(encoding='utf-8')


def _git_files() -> list[str]:
    """Every tracked file, relative to the root."""
    return subprocess.run(
        ['git', 'ls-files'], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.splitlines()


def make_targets() -> set[str]:
    """Every target Makefile declares."""
    return set(re.findall(r'^([a-z][a-z0-9-]*):', _read('Makefile'), re.MULTILINE))


def documented_make_targets() -> dict[str, list[str]]:
    """Every `make <target>` each user document names, by document."""
    found: dict[str, list[str]] = {}
    for rel in USER_DOCS:
        text = _read(rel)
        fences = '\n'.join(re.findall(r'```[a-z]*\n(.*?)```', text, re.DOTALL))
        spans = '\n'.join(re.findall(r'`([^`\n]+)`', text))
        targets = MAKE_TOKEN.findall(fences + '\n' + spans)
        if targets:
            found[rel] = targets
    return found


def check_make_targets() -> list[str]:
    """A documented target Makefile does not declare."""
    known = make_targets()
    problems = []
    for rel, targets in documented_make_targets().items():
        for target in sorted(set(targets)):
            if target in known:
                continue
            problems.append(f'{rel}: `make {target}` matches no Makefile target')
    return problems


def ci_jobs() -> list[str]:
    """Every job ci.yml runs, in file order."""
    with open(ROOT / CI, encoding='utf-8') as handle:
        workflow = yaml.safe_load(handle)
    return list(workflow['jobs'])


def table_first_cells(text: str) -> set[str]:
    """The backticked first cells of every Markdown table row."""
    return set(re.findall(r'^\|\s*`([^`]+)`\s*\|', text, re.MULTILINE))


def check_contributing_jobs() -> list[str]:
    """A CI job CONTRIBUTING maps to no local target, or a row mapping a phantom job."""
    jobs = ci_jobs()
    text = _read('CONTRIBUTING.md')
    named = table_first_cells(text)
    problems = []
    for job in jobs:
        if job.endswith(SAVE_SUFFIX) or job in TWINLESS_JOBS or job in named:
            continue
        problems.append(f'CONTRIBUTING.md: the {job} job has no documented local target')
    for cell in sorted(named):
        if cell not in jobs:
            problems.append(f'CONTRIBUTING.md: `{cell}` is no ci.yml job')
    for job, reason in sorted(TWINLESS_JOBS.items()):
        if job not in jobs:
            problems.append(
                f'CONTRIBUTING.md: `{job}` is described but ci.yml defines no such job'
            )
        if job not in text:
            problems.append(
                f'CONTRIBUTING.md: `{job}` twinless exemption lost its reason: {reason}'
            )
    if any(job.endswith(SAVE_SUFFIX) for job in jobs) and '`-save`' not in text:
        problems.append('CONTRIBUTING.md: the -save jobs lost their twinless sentence')
    return problems


def design_ci_jobs_cell() -> str:
    """The ci.yml row's Jobs cell, the design's inventory of the suite."""
    text = _read('docs/00-design.md')
    section = text[text.index('## CI on GitHub Actions') :]
    for line in section.splitlines():
        if line.startswith('| `ci.yml`'):
            return line
    raise AssertionError('the design CI table names no ci.yml row')


def check_design_jobs() -> list[str]:
    """A ci.yml job the design's CI table does not name."""
    cell = design_ci_jobs_cell()
    named = set(re.findall(r'`([^`]+)`', cell))
    return [
        f'docs/00-design.md: the CI table misses the {job} job'
        for job in ci_jobs()
        if job not in named
    ]


def documented_scenarios() -> dict[str, str]:
    """The numbered entries of section 10, each with its text."""
    text = _read('docs/01-testing.md')
    section = text[text.index('## 10. Key end-to-end scenarios') : text.index('## 11.')]
    entries: dict[str, str] = {}
    for line in section.splitlines():
        found = re.match(r'^(\d+)\. (.*)', line)
        if found:
            entries[found.group(1)] = found.group(2)
    return entries


def scenario_numbers() -> set[str]:
    """The scenario numbers a test file under tests/e2e carries."""
    numbers = set()
    for path in (ROOT / 'tests' / 'e2e').glob('test_scenario_*_*.py'):
        found = re.match(r'test_scenario_(\d+)_', path.name)
        if found:
            numbers.add(found.group(1))
    return numbers


def avr_scenarios() -> set[str]:
    """The scenarios run-avr.sh runs, read from its loop rather than a list."""
    text = (ROOT / 'tests' / 'e2e' / 'run-avr.sh').read_text(encoding='utf-8')
    line = re.search(r'for scenario in ([0-9 ]+);', text)
    if not line:
        raise AssertionError('tests/e2e/run-avr.sh names no scenario loop')
    return set(line.group(1).split())


# An entry with no end-to-end file says so; the phrase is the marker.
NO_E2E = 'no end-to-end scenario'


def check_scenarios() -> list[str]:
    """A scenario file no entry documents, an entry no file backs, or an avr run unnumbered."""
    documented = documented_scenarios()
    files = scenario_numbers()
    problems = [
        f'docs/01-testing.md: scenario {number} runs in CI but section 10 does not list it'
        for number in sorted(files - set(documented), key=int)
    ]
    problems += [
        f'docs/01-testing.md: scenario {number} has no test file and does not say so'
        for number, entry in sorted(documented.items(), key=lambda item: int(item[0]))
        if number not in files and NO_E2E not in entry
    ]
    problems += [
        f'docs/01-testing.md: run-avr.sh runs scenario {number}, which section 10 does not list'
        for number in sorted(avr_scenarios() - set(documented), key=int)
    ]
    return problems


def layout_paths() -> list[str]:
    """The paths the design's Part II layout diagram claims to name."""
    text = _read('docs/00-design.md')
    start = text.index('## Repository layout')
    fence = text.index('```', start)
    return [
        line for line in text[fence : text.index('```', fence + 3)].splitlines() if line.strip()
    ]


def directory_names() -> set[str]:
    """Every tracked directory's top-level name."""
    names = set()
    for path in _git_files():
        parts = Path(path).parts
        if len(parts) > 1:
            names.add(parts[0])
    return names


def check_layout_diagram() -> list[str]:
    """A tracked top-level directory the layout diagram does not name."""
    diagram = '\n'.join(layout_paths())
    skip = {'build', 'install', 'log', 'scratch', '__pycache__'}
    return [
        f'docs/00-design.md: the layout diagram omits the {name}/ directory'
        for name in sorted(directory_names())
        if name not in skip and f'{name}/' not in diagram
    ]


def code_text() -> str:
    """Every tracked file that can define a macro, concatenated."""
    text = []
    for path in _git_files():
        if path in USER_DOCS or path.startswith(('spec/vectors/', 'gen/tests/golden/')):
            continue
        try:
            text.append((ROOT / path).read_text(encoding='utf-8'))
        except (OSError, UnicodeDecodeError):
            continue
    return '\n'.join(text)


def documented_macros() -> dict[str, list[str]]:
    """Every R2S_ token each user document names."""
    found: dict[str, list[str]] = {}
    for rel in USER_DOCS:
        tokens = re.findall(r'R2S_[A-Z0-9_]+', _read(rel))
        if tokens:
            found[rel] = tokens
    return found


def check_macros() -> list[str]:
    """A macro the documentation names that no source carries."""
    code = code_text()
    problems = []
    for rel, tokens in documented_macros().items():
        for token in sorted(set(tokens)):
            if token in MACRO_PLACEHOLDERS or token in code:
                continue
            problems.append(f'{rel}: `{token}` is in no source file')
    return problems


def schema_keys() -> tuple[set[str], dict[str, set[str]]]:
    """The configuration schema's top-level keys and each parent's children."""
    import json

    with open(ROOT / SCHEMA, encoding='utf-8') as handle:
        schema = json.load(handle)
    top = set(schema.get('properties', {}))
    nested: dict[str, set[str]] = {}
    for name, value in schema.get('properties', {}).items():
        item = value.get('items', {})
        ref = item.get('$ref')
        if ref:
            definition = schema['$defs'][ref.rsplit('/', 1)[-1]]
            nested[name] = set(definition.get('properties', {}))
        elif value.get('properties'):
            nested[name] = set(value['properties'])
    return top, nested


def table_keys(text: str) -> set[str]:
    """The keys the design's config table documents, as `topics.name` paths."""
    rows = set(re.findall(r'^\|\s*`([A-Za-z0-9_.\[\]]+)`', text, re.MULTILINE))
    return {row.replace('[]', '') for row in rows}


def check_config_table() -> list[str]:
    """A schema key the design's config table omits, or a row the schema rejects."""
    text = _read('docs/00-design.md')
    section = text[text.index('### Config schema') : text.index('### Type hash')]
    documented = table_keys(section)
    top, nested = schema_keys()
    expected = set(top)
    for parent, children in nested.items():
        expected |= {f'{parent}.{child}' for child in children}
    # A parent row is implied by its children: the table documents
    # `topics[].name` rather than a bare `topics` type.
    documented |= {row.split('.')[0] for row in documented}
    problems = [
        f'docs/00-design.md: r2s.yaml accepts `{key}`, which the config table omits'
        for key in sorted(expected - documented)
    ]
    problems += [
        f'docs/00-design.md: the config table documents `{row}`, which r2s.yaml rejects'
        for row in sorted(documented - expected)
    ]
    return problems


def example_directories() -> set[str]:
    """The example directories a contributor can build."""
    return {
        path.name
        for path in (ROOT / 'examples').iterdir()
        if path.is_dir() and path.name != 'tests'
    }


def check_examples() -> list[str]:
    """An example directory examples/README.md never names."""
    text = _read('examples/README.md')
    problems = []
    for name in sorted(example_directories()):
        short = name.removeprefix('arduino_')
        if name not in text and f'`{short}`' not in text:
            problems.append(f'examples/README.md: the {name} example is not listed')
    return problems


CHECKS = (
    check_make_targets,
    check_contributing_jobs,
    check_design_jobs,
    check_scenarios,
    check_layout_diagram,
    check_macros,
    check_config_table,
    check_examples,
)


def problems() -> list[str]:
    """Every drift the checks find, in check order."""
    found: list[str] = []
    for check in CHECKS:
        found.extend(check())
    return found


def main(argv: list[str] | None = None) -> int:
    """Report the drift; exit 1 naming each problem."""
    parser = argparse.ArgumentParser(description='Check the documentation against the tree.')
    parser.parse_args(argv)
    found = problems()
    for problem in found:
        print(problem)
    print(f'docs-drift: {len(CHECKS)} checks, {len(found)} problems')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
