# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The documentation names the commands, macros, paths, jobs and scenarios the tree has.

Every check reads the tree, never a list kept here, so a renamed target, a
dropped macro, a new CI job or a new scenario fails the gate until the prose
follows. A page that calls a target unbuilt while the Makefile implements it
fails too, so the prose cannot outlive the gap it described. Exemptions live
in the tuples below with the reason they apply.
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

# The file types that can define a macro, a target or a key. A prose page
# that merely mentions a token never proves the token exists.
CODE_SUFFIXES = (
    '.c',
    '.h',
    '.cpp',
    '.hpp',
    '.py',
    '.sh',
    '.yml',
    '.yaml',
    '.txt',
    '.cmake',
)

USER_DOCS_HEAD = ('README.md', 'CONTRIBUTING.md', 'AGENTS.md', 'examples/README.md')


def user_docs() -> tuple[str, ...]:
    """Every prose page the gate reads: the named roots plus every page under docs/."""
    pages = [path for path in USER_DOCS_HEAD if (ROOT / path).is_file()]
    pages += sorted(
        str(path.relative_to(ROOT)) for path in (ROOT / 'docs').rglob('*.md') if path.is_file()
    )
    return tuple(pages)


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
# A job name: lowercase words joined by dashes, no path, image or label shape.
JOB_NAME = re.compile(r'^[a-z][a-z0-9]*(-[a-z0-9]+)*$')
# Tokens shaped like a job name that the CI cell legitimately backs.
CELL_NOT_A_JOB = {
    'continue-on-error': 'the job option the advisory ament-lint job sets',
    'fuzz-long': 'the pull request label that opts into the long fuzzing',
    'master': 'the branch the checksum cache saves under',
}


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
    for rel in user_docs():
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


# A target the Makefile declares but does not implement refuses through
# `$(call pending,<target>,<reason>)`, which names the work that will land it.
PENDING_RECIPE = '$(call pending,'
# The wording a page uses when it means a command does not run yet. "Built"
# alone is the firmware verb, so the phrase needs the negation.
UNBUILT = re.compile(r"\bnot (?:yet )?built\b|\bisn't built\b|\baren't built\b", re.IGNORECASE)
# The target a claim names: the word after `make`, or the first word of a
# backticked span, which is how a table row's first cell spells it. Only a
# name the Makefile implements is judged, so prose like "make its own
# decision" never matches a target.
MAKE_WORD = re.compile(r'\bmake ([a-z][a-z0-9-]*)')
SPAN = re.compile(r'`([^`\n]+)`')
# A numbered list item and a heading start their own block: a claim in one
# item must not take a command named in the next as its own.
ITEM_START = re.compile(r'^(?:\d+[.)]|#{1,6})\s')


def claimed_targets(text: str) -> set[str]:
    """Every target name a block writes, however it spells it."""
    targets = set(MAKE_WORD.findall(text))
    for span in SPAN.findall(text):
        words = span.split()
        if not words:
            continue
        # A span holding the whole command spells it as `make <target>`.
        targets.add(words[1] if words[0] == 'make' and len(words) > 1 else words[0])
    targets.discard('make')
    return targets


def built_targets() -> set[str]:
    """Every target with a recipe of its own, leaving out one that refuses with `pending`.

    A page that calls a command unbuilt points a reader away from a target
    that works, which is the drift the validation page carried after
    `hardware-check` shipped.
    """
    text = _read('Makefile')
    built: set[str] = set()
    for name in make_targets():
        found = re.search(rf'^{re.escape(name)}:[^\n]*\n((?:\t[^\n]*\n)+)', text, re.MULTILINE)
        if found and PENDING_RECIPE not in found.group(1):
            built.add(name)
    return built


def claim_blocks(text: str) -> list[tuple[int, str]]:
    """Each bullet, numbered item, heading or prose paragraph, and each table row.

    Pages wrap prose, so the sentence that calls something unbuilt and the
    command it names land on different lines of one bullet. A paragraph would
    swallow a whole table: one row names an unbuilt thing while the next row
    mentions a command.
    """
    blocks: list[tuple[int, str]] = []
    start, lines = 1, []

    def flush() -> None:
        nonlocal start, lines
        if lines:
            blocks.append((start, '\n'.join(lines)))
            lines = []

    for number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not stripped:
            flush()
        elif stripped.startswith('|'):
            flush()
            blocks.append((number, line))
        elif stripped.startswith(('- ', '* ')) or ITEM_START.match(stripped):
            flush()
            start, lines = number, [line]
        else:
            if not lines:
                start = number
            lines.append(line)
    flush()
    return blocks


def check_unbuilt_claims() -> list[str]:
    """A block calling a target unbuilt while the Makefile implements it.

    The target must be in the block, written the way the pages write it: after
    `make`, or as a table row's first cell. Nothing else ties "the board
    check" to `hardware-check`, and guessing that link would fail pages about
    work that really is unbuilt.
    """
    built = built_targets()
    problems: set[str] = set()
    for rel in user_docs():
        for start, block in claim_blocks(_read(rel)):
            if not UNBUILT.search(block):
                continue
            problems |= {
                f'{rel}:{start}: `{target}` is called not built, but the Makefile implements it'
                for target in claimed_targets(block)
                if target in built
            }
    return sorted(problems)


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
    # The reasons live in the prose below the table, so the search skips the
    # table itself: a job row is not an excuse for having no local target.
    prose = '\n'.join(line for line in text.splitlines() if not line.startswith('|'))
    for job, reason in sorted(TWINLESS_JOBS.items()):
        if job not in jobs:
            problems.append(
                f'CONTRIBUTING.md: `{job}` is described but ci.yml defines no such job'
            )
        if f'`{job}`' not in prose:
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
    """A ci.yml job the design's CI table does not name, or a row naming a phantom job."""
    cell = design_ci_jobs_cell()
    named = set(re.findall(r'`([^`]+)`', cell))
    jobs = ci_jobs()
    problems = [
        f'docs/00-design.md: the CI table misses the {job} job' for job in jobs if job not in named
    ]
    # The cell also backs paths, images and labels, so only a token shaped like
    # a job name (lowercase, dashes, no separator) can claim to be one.
    problems += [
        f'docs/00-design.md: the CI table names `{name}`, which is no ci.yml job'
        for name in sorted(named - set(jobs))
        if JOB_NAME.match(name) and name not in CELL_NOT_A_JOB
    ]
    return problems


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


def root_entries() -> tuple[set[str], set[str]]:
    """The tree's top-level directories and every tracked top-level name."""
    directories: set[str] = set()
    names: set[str] = set()
    for path in _git_files():
        parts = Path(path).parts
        names.add(parts[0])
        if len(parts) > 1:
            directories.add(parts[0])
    return directories, names


def check_layout_diagram() -> list[str]:
    """A tracked top-level directory the layout diagram does not name."""
    diagram = '\n'.join(layout_paths())
    skip = {'build', 'install', 'log', 'scratch', '__pycache__'}
    directories, tracked = root_entries()
    # Column zero is the root of the tree: a nested mention of the name, or a
    # path inside it like `tools/vectors_check.py`, is not the entry.
    drawn = set(
        re.findall(r'^[\u251c\u2514]\u2500\u2500 ([A-Za-z_.][\w.-]*)', diagram, re.MULTILINE)
    )
    problems = [
        f'docs/00-design.md: the layout diagram omits the {name}/ directory'
        for name in sorted(directories)
        if name not in skip and name not in drawn
    ]
    # Two-way: a name the diagram puts at the root that the tree lacks is the
    # same defect, and the trailing slash is optional because the invented
    # entry this check exists for carried none.
    problems += [
        f'docs/00-design.md: the layout diagram names the {name}/ directory, which the tree lacks'
        for name in sorted(drawn - tracked)
        if name not in skip
    ]
    return problems


def code_text() -> str:
    """Every tracked file that can define a macro, concatenated."""
    text = []
    for path in _git_files():
        if path.startswith('gen/tests/golden/') or path.startswith('spec/vectors/'):
            continue
        if not (path.endswith(CODE_SUFFIXES) or path == 'spec/protocol.md'):
            continue
        try:
            text.append((ROOT / path).read_text(encoding='utf-8'))
        except (OSError, UnicodeDecodeError):
            continue
    return '\n'.join(text)


def documented_macros() -> dict[str, list[str]]:
    """Every R2S_ token each user document names."""
    found: dict[str, list[str]] = {}
    for rel in user_docs():
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


def _example_like(names: set[str]) -> set[str]:
    """The backticked names shaped like a sketch: the table's rows and the prose's sketch names."""
    return {name for name in names if name.startswith(('arduino_', 'cmake_', 'pio_', 'ch32v_'))}


def check_examples() -> list[str]:
    """An example directory the README table never names, or a row no directory backs."""
    text = _read('examples/README.md')
    # Backticked names, matched whole: this page lists one row for several
    # sketches and names the non-Arduino ones in prose.
    listed = set(re.findall(r'`([^`\n]+)`', text))
    short_names = {name.removeprefix('arduino_'): name for name in example_directories()}
    problems = [
        f'examples/README.md: the {name} example is not listed'
        for short, name in sorted(short_names.items())
        if short not in listed and name not in listed
    ]
    names = set(short_names) | set(short_names.values())
    # Only a listed row claims to be a sketch; prose and paths name other things.
    problems += [
        f'examples/README.md: `{cell}` is no example directory'
        for cell in sorted(listed & set(_example_like(listed)))
        if cell not in names
    ]
    return problems


BOUNDS_PAGE = 'docs/message-bounds.md'
# The message-bound spellings, plus the bare words the prose uses when it means
# a field's storage. A timing or frame-size "bound" is what the negative
# contexts below exclude, so a page about a message field always trips one.
BOUND_TOKEN = re.compile(
    r'BUNDLE_BOUNDS|--bound|unbounded|string<=|T\[<=|\[<=|`bounds`|^bounds:'
    r'|\bbounds?\b(?! *violation)',
    re.MULTILINE | re.IGNORECASE,
)
# A page whose only "bound" is one of these is not about message storage. Each
# entry pairs the phrasing with what it means, so a new exemption is a decision.
BOUND_ELSEWHERE = (
    re.compile(r'\b(?:time|clock|timing|jitter|frame|latency|stack)s? bound\b', re.I),
    re.compile(r'\bbound (?:the|a) (?:frame|tick|time|latency)\b', re.I),
    re.compile(r'padding-blind lower bound\b', re.I),
    re.compile(r'proves the bound\b', re.I),
    re.compile(r'\bstack bound\b|\bbound-sized\b', re.I),
    # Memory safety, not a field's storage: a decoder bounds-checks a buffer.
    re.compile(r'out-of-bounds|\bbounds check\b', re.I),
    # A benchmark page measures sizes, so its "bounds" are the capacities the
    # tables are computed at rather than a reader's choice to change.
    re.compile(r"bundle's bounds|documented bounds|field and bound choices", re.I),
)


def bounds_heading_ids(text: str) -> set[str]:
    """The anchors the bounds page's Markdown headings produce, GitHub style."""
    ids = set()
    for line in text.splitlines():
        matched = re.match(r'^#{1,6}\s+(.*?)\s*$', line)
        if not matched:
            continue
        heading = re.sub(r'<[^>]*>', '', matched.group(1)).lower()
        heading = re.sub(r'[^a-z0-9 _-]', '', heading).strip()
        ids.add(re.sub(r'\s+', '-', heading))
    return ids


def states_a_bound(text: str) -> bool:
    """Whether a page states a message bound rather than a timing or size limit."""
    stripped = text
    for pattern in BOUND_ELSEWHERE:
        stripped = pattern.sub(' ', stripped)
    return bool(BOUND_TOKEN.search(stripped))


# A Markdown link or reference target, with an optional anchor and title. The
# destination is either angle-bracketed or a bare word, and a title after it is
# prose, so neither form hides a correct link from the check.
LINK = re.compile(
    r'\]\(\s*(?:<([^>\s]+)>|([^)\s]+))(?:\s+(?:"[^"]*"|\'[^\']*\'|\([^)]*\)))?\s*\)'
    r'|^ {0,3}\[[^]]+\]:\s*(?:<([^>\s]+)>|(\S+))',
    re.MULTILINE,
)


def _linked_page(rel: str, target: str) -> str | None:
    """The tree path a link on page `rel` reaches, or None when it leaves the tree."""
    path = target.split('#', 1)[0]
    if not path or ':' in path:
        return None
    parts = (Path(rel).parent / path).parts
    joined: list[str] = []
    for part in parts:
        if part == '..':
            if not joined:
                return None
            joined.pop()
        elif part != '.':
            joined.append(part)
    return '/'.join(joined)


def check_bounds_links() -> list[str]:
    """A page stating a bound that does not link the section, or a dead link to it."""
    page = ROOT / BOUNDS_PAGE
    headings = bounds_heading_ids(page.read_text(encoding='utf-8')) if page.is_file() else set()
    problems = []
    for rel in user_docs():
        if rel == BOUNDS_PAGE:
            continue
        text = _read(rel)
        # The link is judged where it is written: a nested page reaches the
        # section as ../message-bounds.md, and only reaching the section counts.
        targets = [
            next((part for part in found.groups() if part), '') for found in LINK.finditer(text)
        ]
        links = [target for target in targets if _linked_page(rel, target) == BOUNDS_PAGE]
        if not links and states_a_bound(text):
            problems.append(f'{rel}: mentions bounds but does not link {BOUNDS_PAGE}')
        for target in links:
            fragment = target.partition('#')[2]
            if not page.is_file():
                problems.append(f'{rel}: link to {target} names no page')
                continue
            if fragment and fragment not in headings:
                problems.append(f'{rel}: link to {target} matches no heading on the bounds page')
    return problems


SITE = 'https://apojomovsky.github.io/ros2serial/'
SITE_URL = re.compile(re.escape(SITE) + r"""([^\s'"`)]+)""")
# The generator's sources are where a message names a reader-facing page.
POINTER_SOURCES = ('gen/r2s_gen',)


def published_pages() -> set[str]:
    """The files the docs site publishes, read from the builder's page table."""
    import importlib.util

    # The builder is loaded from the tree under test, not from whatever
    # site_build this interpreter already imported, so a fixture's page table
    # is the one that judges its fixture's URLs.
    path = ROOT / 'tools' / 'site_build.py'
    if not path.is_file():
        return set()
    spec = importlib.util.spec_from_file_location('_site_build_under_test', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return {page.output for page in module.PAGES} | {'index.html'}


def check_docs_urls() -> list[str]:
    """A site URL the generator prints that names no page the site publishes."""
    pages = published_pages()
    problems = []
    for directory in POINTER_SOURCES:
        for path in sorted((ROOT / directory).rglob('*.py')):
            rel = str(path.relative_to(ROOT))
            for found in SITE_URL.finditer(path.read_text(encoding='utf-8')):
                if found.group(1) not in pages:
                    problems.append(f'{rel}: {found.group(1)} is no page the site publishes')
    return problems


# The pages a reader starts from: the front page, the contributor guide and the
# rules, which link onward. A page none of them reaches is orphaned for a reader
# who does not browse the site's navigation.
ENTRY_PAGES = ('README.md', 'CONTRIBUTING.md', 'AGENTS.md')


def page_graph() -> dict[str, set[str]]:
    """Every prose page, with the in-tree pages it links."""
    pages = user_docs()
    known = set(pages)
    graph: dict[str, set[str]] = {}
    for rel in pages:
        text = _read(rel)
        targets = {
            page
            for page in (
                _linked_page(rel, next(part for part in found.groups() if part))
                for found in LINK.finditer(text)
            )
            if page in known and page != rel
        }
        graph[rel] = targets
    return graph


def check_pages_reachable() -> list[str]:
    """A page no entry page reaches, so a reader following links never lands there."""
    graph = page_graph()
    reached = {page for page in ENTRY_PAGES if page in graph}
    queue = list(reached)
    while queue:
        for target in graph[queue.pop()]:
            if target not in reached:
                reached.add(target)
                queue.append(target)
    return [
        f'{page}: no page a reader starts from reaches this one'
        for page in sorted(set(graph) - reached)
    ]


# The generator's subcommand name in a documented command, whatever wraps it:
# prose writes `r2s-gen generate`, a terminal writes `ros2 run r2s_gen r2s-gen
# generate`, and both name the same parser entry.
GENERATOR_COMMAND = re.compile(r'\br2s-gen ([a-z][a-z0-9-]*)')
ARDUINO_COMMAND = re.compile(r'\br2s-arduino ([a-z][a-z0-9-]*)')
SUBCOMMAND = re.compile(r"add_parser\(\s*'([a-z][a-z0-9-]*)'")


def generator_subcommands(script: str) -> set[str]:
    """Every subcommand the named console script's parser declares."""
    found = set()
    for rel in _git_files():
        if not rel.endswith('.py') or not rel.startswith('gen/'):
            continue
        text = _read(rel)
        if script == 'r2s-arduino' and 'commands.add_parser(' not in text:
            continue
        if script == 'r2s-gen' and 'subparsers.add_parser(' not in text:
            continue
        found |= set(SUBCOMMAND.findall(text))
    return found


def documented_commands(pattern: re.Pattern) -> dict[str, list[str]]:
    """Every subcommand each document names for one console script."""
    found: dict[str, list[str]] = {}
    for rel in user_docs():
        names = pattern.findall(_read(rel))
        if names:
            found[rel] = names
    return found


def check_generator_commands() -> list[str]:
    """A subcommand the documentation names and the generator's parser lacks."""
    problems = []
    for script, pattern in (
        ('r2s-gen', GENERATOR_COMMAND),
        ('r2s-arduino', ARDUINO_COMMAND),
    ):
        known = generator_subcommands(script)
        for rel, names in documented_commands(pattern).items():
            problems += [
                f'{rel}: `{script} {name}` is no generator subcommand'
                for name in sorted(set(names))
                if name not in known
            ]
    return problems


# A pair as a reader types it: the package, then the executable it installs.
ROS2_RUN = re.compile(r'\bros2 run ([a-z][a-z0-9_]*) ([A-Za-z][A-Za-z0-9_-]*)')
CONSOLE_SCRIPT = re.compile(r"'([A-Za-z][A-Za-z0-9_-]*) = ")


def installed_executables() -> dict[str, set[str]]:
    """Each ROS package's installed executables, from its manifest and its install rules."""
    packages: dict[str, set[str]] = {}
    for manifest in _git_files():
        if not manifest.endswith('package.xml'):
            continue
        text = _read(manifest)
        found = re.search(r'<name>([^<]+)</name>', text)
        if not found:
            continue
        name = found.group(1)
        root = Path(manifest).parent
        names: set[str] = set()
        setup = ROOT / root / 'setup.py'
        if setup.is_file():
            names |= set(CONSOLE_SCRIPT.findall(setup.read_text(encoding='utf-8')))
        cmake = ROOT / root / 'CMakeLists.txt'
        if cmake.is_file():
            names |= set(
                re.findall(
                    r'install\(\s*TARGETS\s+([A-Za-z0-9_]+)[^)]*RUNTIME DESTINATION',
                    cmake.read_text(encoding='utf-8'),
                )
            )
        packages[name] = names
    return packages


def check_ros2_run() -> list[str]:
    """A `ros2 run` pair whose package or executable the tree does not install."""
    packages = installed_executables()
    problems = []
    for rel in user_docs():
        text = _read(rel)
        for package, executable in sorted(set(ROS2_RUN.findall(text))):
            if package not in packages:
                problems.append(f'{rel}: `ros2 run {package} {executable}` names no package')
            elif executable not in packages[package]:
                problems.append(
                    f'{rel}: `ros2 run {package} {executable}` names no installed executable'
                )
    return problems


CHECKS = (
    check_make_targets,
    check_unbuilt_claims,
    check_contributing_jobs,
    check_design_jobs,
    check_scenarios,
    check_layout_diagram,
    check_macros,
    check_config_table,
    check_examples,
    check_bounds_links,
    check_docs_urls,
    check_pages_reachable,
    check_generator_commands,
    check_ros2_run,
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
