# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

import json

import coverage_gate


def write_summary(path, line_covered, line_total, branch_covered, branch_total):
    payload = {
        'line_covered': line_covered,
        'line_total': line_total,
        'branch_covered': branch_covered,
        'branch_total': branch_total,
    }
    path.write_text(json.dumps(payload))
    return str(path)


def errors(component, head, advisory=frozenset()):
    problems, _ = coverage_gate.check_files(component, head, advisory)
    return problems


def test_equal_figures_pass(tmp_path):
    head = write_summary(tmp_path / 'head.json', 96, 100, 90, 100)
    assert coverage_gate.check_files('core', head) == ([], [])


def test_a_raised_figure_passes(tmp_path):
    head = write_summary(tmp_path / 'head.json', 97, 100, 91, 100)
    assert coverage_gate.check_files('core', head) == ([], [])


def test_below_the_floor_fails(tmp_path):
    head = write_summary(tmp_path / 'head.json', 94, 100, 90, 100)
    assert any('floor' in p for p in errors('core', head))


def test_an_advisory_floor_is_reported_without_failing(tmp_path):
    head = write_summary(tmp_path / 'head.json', 94, 100, 90, 100)
    problems, advisories = coverage_gate.check_files('core', head, {'core'})
    assert problems == [] and any('floor' in a for a in advisories)


def test_no_base_checks_only_the_floor(tmp_path):
    head = write_summary(tmp_path / 'head.json', 96, 100, 90, 100)
    assert coverage_gate.check_files('core', head) == ([], [])


def test_an_unknown_component_is_a_problem(tmp_path):
    head = write_summary(tmp_path / 'head.json', 96, 100, 90, 100)
    problems, _ = coverage_gate.check_files('firmware', head)
    assert problems != []


def test_gen_at_the_floors_passes(tmp_path):
    head = write_summary(tmp_path / 'head.json', 95, 100, 90, 100)
    assert coverage_gate.check_files('gen', head) == ([], [])


def test_gen_below_the_floor_fails(tmp_path):
    head = write_summary(tmp_path / 'head.json', 94, 100, 90, 100)
    assert any('floor' in p for p in errors('gen', head))


def test_gen_below_the_branch_floor_fails(tmp_path):
    head = write_summary(tmp_path / 'head.json', 95, 100, 89, 100)
    assert any('floor' in p for p in errors('gen', head))


def test_gen_without_base_checks_only_the_floor(tmp_path):
    head = write_summary(tmp_path / 'head.json', 96, 100, 91, 100)
    assert coverage_gate.check_files('gen', head) == ([], [])


def test_an_empty_measurement_is_a_problem(tmp_path):
    head = write_summary(tmp_path / 'head.json', 0, 0, 0, 0)
    assert errors('core', head) == ['coverage: core measured no lines']


def test_main_reports_one_line_per_floor_failure(tmp_path, capsys):
    head = write_summary(tmp_path / 'head.json', 80, 100, 70, 100)
    assert coverage_gate.main(['--component', 'core', '--head', head]) == 1
    assert len(capsys.readouterr().out.strip().splitlines()) == 2


def test_main_passes_on_an_advisory_floor_breach(tmp_path, capsys):
    head = write_summary(tmp_path / 'head.json', 71, 100, 51, 100)
    argv = ['--component', 'bridge', '--head', head, '--advisory-floor', 'bridge']
    assert coverage_gate.main(argv) == 0
    assert '(advisory)' in capsys.readouterr().out


def write_gcovr_detail(path, files):
    """Write a gcovr --json detail file.

    files maps a repo-relative path to a list of
    (line_number, count, excluded, [(branch_count, branch_excluded), ...]).
    """
    doc = {'gcovr/format_version': '0.10', 'files': []}
    for rel, lines in files.items():
        entry = {'file': rel, 'functions': [], 'lines': []}
        for number, count, excluded, branches in lines:
            item = {'line_number': number, 'count': count, 'branches': []}
            for branch_count, branch_excluded in branches:
                branch = {'count': branch_count}
                if branch_excluded:
                    branch['gcovr/excluded'] = True
                item['branches'].append(branch)
            if excluded:
                item['gcovr/excluded'] = True
            entry['lines'].append(item)
        doc['files'].append(entry)
    path.write_text(json.dumps(doc))
    return str(path)


def write_coveragepy(path, files):
    """Write a coverage.py JSON report.

    files maps a path to
    (executed, missing, excluded, executed_arcs, missing_arcs).
    """
    doc_files = {}
    covered_lines = 0
    num_statements = 0
    covered_branches = 0
    num_branches = 0
    for rel, (executed, missing, excluded, exec_arcs, missing_arcs) in files.items():
        doc_files[rel] = {
            'executed_lines': sorted(executed),
            'missing_lines': sorted(missing),
            'excluded_lines': sorted(excluded),
            'executed_branches': [list(arc) for arc in exec_arcs],
            'missing_branches': [list(arc) for arc in missing_arcs],
            'summary': {},
        }
        covered_lines += len(executed)
        num_statements += len(executed) + len(missing)
        covered_branches += len(exec_arcs)
        num_branches += len(exec_arcs) + len(missing_arcs)
    doc = {
        'meta': {'format': 3},
        'files': doc_files,
        'totals': {
            'covered_lines': covered_lines,
            'num_statements': num_statements,
            'covered_branches': covered_branches,
            'num_branches': num_branches,
        },
    }
    path.write_text(json.dumps(doc))
    return str(path)


def write_source(root, rel, text):
    target = root / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)
    return str(target)


def loader(root):
    def load(path):
        target = root / path
        if not target.is_file():
            return None
        return target.read_text().splitlines()

    return load


def check_patch(component, root, detail_path, diff_text, measured=None):
    detail = coverage_gate.read_detail(component, detail_path)
    changed = coverage_gate.parse_diff(diff_text)
    if measured is None:
        measured = set(changed)
    return coverage_gate.check_patch(component, detail, changed, measured, loader(root))


def test_parse_diff_collects_added_lines_under_the_new_path():
    text = (
        'diff --git a/core/src/f.c b/core/src/f.c\n'
        '--- a/core/src/f.c\n'
        '+++ b/core/src/f.c\n'
        '@@ -1,3 +1,4 @@\n'
        ' int f(void) {\n'
        '-  return 0;\n'
        '+  return 1;\n'
        '+  // a new comment\n'
        ' }\n'
    )
    assert coverage_gate.parse_diff(text) == {'core/src/f.c': [2, 3]}


def test_parse_diff_ignores_a_deleted_file():
    text = (
        'diff --git a/old.c b/old.c\n'
        'deleted file mode 100644\n'
        '--- a/old.c\n'
        '+++ /dev/null\n'
        '@@ -1,2 +0,0 @@\n'
        '-int f(void) {\n'
        '-}\n'
    )
    assert coverage_gate.parse_diff(text) == {}


def test_parse_diff_tracks_a_new_file():
    text = (
        'diff --git a/core/src/n.c b/core/src/n.c\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/core/src/n.c\n'
        '@@ -0,0 +1,2 @@\n'
        '+int g(void) {\n'
        '+}\n'
    )
    assert coverage_gate.parse_diff(text) == {'core/src/n.c': [1, 2]}


def test_patch_passes_when_changed_lines_are_covered_and_the_package_dips(tmp_path):
    write_source(tmp_path, 'core/src/f.c', 'int f(void) {\n  return 1;\n}\n')
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {'core/src/f.c': [(1, 2, False, []), (2, 2, False, [])]},
    )
    diff = (
        'diff --git a/core/src/f.c b/core/src/f.c\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/core/src/f.c\n'
        '@@ -0,0 +1,3 @@\n'
        '+int f(void) {\n'
        '+  return 1;\n'
        '+}\n'
    )
    problems, figure, _ = check_patch('core', tmp_path, detail, diff)
    assert problems == []
    assert (figure.lines_changed, figure.lines_covered, figure.lines_excluded) == (2, 2, 0)


def test_patch_fails_naming_the_uncovered_changed_line(tmp_path):
    write_source(tmp_path, 'core/src/f.c', 'int f(void) {\n  return 1;\n}\n')
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {'core/src/f.c': [(1, 2, False, []), (2, 0, False, [])]},
    )
    diff = (
        'diff --git a/core/src/f.c b/core/src/f.c\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/core/src/f.c\n'
        '@@ -0,0 +1,3 @@\n'
        '+int f(void) {\n'
        '+  return 1;\n'
        '+}\n'
    )
    problems, _, _ = check_patch('core', tmp_path, detail, diff)
    assert problems == ['coverage: core patch: core/src/f.c:2: uncovered line']


def test_patch_accepts_a_reasoned_line_exclusion(tmp_path):
    write_source(
        tmp_path,
        'core/src/f.c',
        'int f(void) {\n  return -1;  // GCOV_EXCL_LINE: callers clamp\n}\n',
    )
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {'core/src/f.c': [(1, 2, False, []), (2, 0, True, [])]},
    )
    diff = (
        'diff --git a/core/src/f.c b/core/src/f.c\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/core/src/f.c\n'
        '@@ -0,0 +1,3 @@\n'
        '+int f(void) {\n'
        '+  return -1;  // GCOV_EXCL_LINE: callers clamp\n'
        '+}\n'
    )
    problems, figure, _ = check_patch('core', tmp_path, detail, diff)
    assert problems == []
    assert (figure.lines_changed, figure.lines_covered, figure.lines_excluded) == (2, 1, 1)


def test_patch_rejects_an_exclusion_without_a_reason(tmp_path):
    write_source(tmp_path, 'core/src/f.c', 'int f(void) {\n  return -1;  // GCOV_EXCL_LINE\n}\n')
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {'core/src/f.c': [(1, 2, False, []), (2, 0, True, [])]},
    )
    diff = (
        'diff --git a/core/src/f.c b/core/src/f.c\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/core/src/f.c\n'
        '@@ -0,0 +1,3 @@\n'
        '+int f(void) {\n'
        '+  return -1;  // GCOV_EXCL_LINE\n'
        '+}\n'
    )
    problems, _, _ = check_patch('core', tmp_path, detail, diff)
    assert problems == ['coverage: core patch: core/src/f.c:2: excluded without a reason']


def test_patch_accepts_a_reasoned_branch_range(tmp_path):
    write_source(
        tmp_path,
        'core/src/f.c',
        'int f(int x) {\n'
        '  // GCOV_EXCL_BR_START: installed fields always match\n'
        '  if (x < 0) {\n'
        '    return -1;\n'
        '  }\n'
        '  // GCOV_EXCL_BR_STOP\n'
        '  return x;\n'
        '}\n',
    )
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {
            'core/src/f.c': [
                (1, 2, False, []),
                (3, 2, False, [(2, True), (0, True)]),
                (4, 0, True, []),
                (7, 2, False, []),
            ]
        },
    )
    diff = (
        'diff --git a/core/src/f.c b/core/src/f.c\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/core/src/f.c\n'
        '@@ -0,0 +1,8 @@\n'
        '+int f(int x) {\n'
        '+  // GCOV_EXCL_BR_START: installed fields always match\n'
        '+  if (x < 0) {\n'
        '+    return -1;\n'
        '+  }\n'
        '+  // GCOV_EXCL_BR_STOP\n'
        '+  return x;\n'
        '+}\n'
    )
    problems, figure, _ = check_patch('core', tmp_path, detail, diff)
    assert problems == []
    assert figure.lines_excluded == 1
    assert figure.branches_excluded == 2


def test_patch_fails_on_an_uncovered_branch_of_a_covered_line(tmp_path):
    write_source(
        tmp_path,
        'core/src/f.c',
        'int f(int x) {\n  if (x) {\n    return 1;\n  }\n  return 0;\n}\n',
    )
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {
            'core/src/f.c': [
                (1, 2, False, []),
                (2, 2, False, [(0, False), (2, False)]),
                (3, 0, False, []),
                (5, 2, False, []),
            ]
        },
    )
    diff = (
        'diff --git a/core/src/f.c b/core/src/f.c\n'
        '--- a/core/src/f.c\n'
        '+++ b/core/src/f.c\n'
        '@@ -1,5 +1,5 @@\n'
        ' int f(int x) {\n'
        '-  if (y) {\n'
        '+  if (x) {\n'
        '     return 1;\n'
        '   }\n'
        '   return 0;\n'
    )
    problems, _, _ = check_patch('core', tmp_path, detail, diff)
    assert problems == ['coverage: core patch: core/src/f.c:2: uncovered branch']


def test_patch_skips_throw_branches(tmp_path):
    write_source(tmp_path, 'bridge/r2s_bridge/src/f.cpp', 'int f(int x) {\n  return x;\n}\n')
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {'bridge/r2s_bridge/src/f.cpp': [(1, 1, False, []), (2, 1, False, [])]},
    )
    doc = json.loads((tmp_path / 'detail.json').read_text())
    doc['files'][0]['lines'][1]['branches'] = [{'count': 0, 'throw': True, 'fallthrough': False}]
    (tmp_path / 'detail.json').write_text(json.dumps(doc))
    diff = (
        'diff --git a/bridge/r2s_bridge/src/f.cpp b/bridge/r2s_bridge/src/f.cpp\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/bridge/r2s_bridge/src/f.cpp\n'
        '@@ -0,0 +1,3 @@\n'
        '+int f(int x) {\n'
        '+  return x;\n'
        '+}\n'
    )
    problems, figure, _ = check_patch('bridge', tmp_path, detail, diff)
    assert problems == []
    assert figure.branches_changed == 0


def test_patch_ignores_a_changed_file_with_no_coverage_data(tmp_path):
    write_source(tmp_path, 'core/src/ghost.c', 'int g(void) {\n  return 1;\n}\n')
    detail = write_gcovr_detail(tmp_path / 'detail.json', {})
    diff = (
        'diff --git a/core/src/ghost.c b/core/src/ghost.c\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/core/src/ghost.c\n'
        '@@ -0,0 +1,3 @@\n'
        '+int g(void) {\n'
        '+  return 1;\n'
        '+}\n'
    )
    problems, figure, skipped = check_patch('core', tmp_path, detail, diff)
    assert problems == []
    assert figure.lines_changed == 0
    assert skipped == ['core/src/ghost.c']


def test_patch_lists_changed_files_outside_measured_code_as_skipped(tmp_path):
    write_source(tmp_path, 'core/src/f.c', 'int f(void) {\n  return 1;\n}\n')
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {'core/src/f.c': [(1, 2, False, []), (2, 2, False, [])]},
    )
    diff = (
        'diff --git a/core/src/f.c b/core/src/f.c\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/core/src/f.c\n'
        '@@ -0,0 +1,3 @@\n'
        '+int f(void) {\n'
        '+  return 1;\n'
        '+}\n'
        'diff --git a/docs/walk.md b/docs/walk.md\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/docs/walk.md\n'
        '@@ -0,0 +1,1 @@\n'
        '+hi\n'
    )
    problems, figure, skipped = check_patch(
        'core', tmp_path, detail, diff, measured={'core/src/f.c'}
    )
    assert problems == []
    assert figure.lines_changed == 2
    assert skipped == ['docs/walk.md']


def test_gen_patch_passes_when_changed_lines_are_covered_and_the_package_dips(tmp_path):
    write_source(tmp_path, 'gen/r2s_gen/m.py', 'def f(x):\n    return x\n')
    detail = write_coveragepy(
        tmp_path / 'detail.json',
        {'gen/r2s_gen/m.py': ([1, 2], [], [], [], [])},
    )
    diff = (
        'diff --git a/gen/r2s_gen/m.py b/gen/r2s_gen/m.py\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/gen/r2s_gen/m.py\n'
        '@@ -0,0 +1,2 @@\n'
        '+def f(x):\n'
        '+    return x\n'
    )
    problems, figure, _ = check_patch('gen', tmp_path, detail, diff)
    assert problems == []
    assert (figure.lines_changed, figure.lines_covered, figure.lines_excluded) == (2, 2, 0)


def test_gen_patch_fails_naming_the_uncovered_changed_line(tmp_path):
    write_source(
        tmp_path, 'gen/r2s_gen/m.py', 'def f(x):\n    if x:\n        return 1\n    return 0\n'
    )
    detail = write_coveragepy(
        tmp_path / 'detail.json',
        {'gen/r2s_gen/m.py': ([1, 2, 4], [3], [], [[2, 4]], [[2, 3]])},
    )
    diff = (
        'diff --git a/gen/r2s_gen/m.py b/gen/r2s_gen/m.py\n'
        '--- a/gen/r2s_gen/m.py\n'
        '+++ b/gen/r2s_gen/m.py\n'
        '@@ -1,4 +1,4 @@\n'
        ' def f(x):\n'
        '     if x:\n'
        '-        return 1\n'
        '+        return 2\n'
        '     return 0\n'
    )
    problems, _, _ = check_patch('gen', tmp_path, detail, diff)
    assert problems == ['coverage: gen patch: gen/r2s_gen/m.py:3: uncovered line']


def test_gen_patch_fails_on_a_missing_branch_arc_from_a_changed_line(tmp_path):
    write_source(
        tmp_path, 'gen/r2s_gen/m.py', 'def f(x):\n    if x:\n        return 1\n    return 0\n'
    )
    missing = write_coveragepy(
        tmp_path / 'missing.json',
        {'gen/r2s_gen/m.py': ([1, 2, 4], [], [], [[2, 4]], [[2, 3]])},
    )
    diff = (
        'diff --git a/gen/r2s_gen/m.py b/gen/r2s_gen/m.py\n'
        '--- a/gen/r2s_gen/m.py\n'
        '+++ b/gen/r2s_gen/m.py\n'
        '@@ -1,4 +1,4 @@\n'
        ' def f(x):\n'
        '-    if y:\n'
        '+    if x:\n'
        '         return 1\n'
        '     return 0\n'
    )
    problems, _, _ = check_patch('gen', tmp_path, missing, diff)
    assert problems == ['coverage: gen patch: gen/r2s_gen/m.py:2: uncovered branch']


def test_gen_patch_accepts_a_reasoned_pragma(tmp_path):
    write_source(
        tmp_path,
        'gen/r2s_gen/m.py',
        'def f(x):\n    if x:\n'
        '        return 1  # pragma: no cover - callers clamp\n'
        '    return 0\n',
    )
    detail = write_coveragepy(
        tmp_path / 'detail.json',
        {'gen/r2s_gen/m.py': ([1, 2, 4], [], [3], [], [])},
    )
    diff = (
        'diff --git a/gen/r2s_gen/m.py b/gen/r2s_gen/m.py\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/gen/r2s_gen/m.py\n'
        '@@ -0,0 +1,4 @@\n'
        '+def f(x):\n'
        '+    if x:\n'
        '+        return 1  # pragma: no cover - callers clamp\n'
        '+    return 0\n'
    )
    problems, figure, _ = check_patch('gen', tmp_path, detail, diff)
    assert problems == []
    assert (figure.lines_changed, figure.lines_covered, figure.lines_excluded) == (4, 3, 1)


def test_gen_patch_rejects_a_reasonless_pragma(tmp_path):
    write_source(
        tmp_path,
        'gen/r2s_gen/m.py',
        'def f(x):\n    if x:\n        return 1  # pragma: no cover\n    return 0\n',
    )
    detail = write_coveragepy(
        tmp_path / 'detail.json',
        {'gen/r2s_gen/m.py': ([1, 2, 4], [], [3], [], [])},
    )
    diff = (
        'diff --git a/gen/r2s_gen/m.py b/gen/r2s_gen/m.py\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/gen/r2s_gen/m.py\n'
        '@@ -0,0 +1,4 @@\n'
        '+def f(x):\n'
        '+    if x:\n'
        '+        return 1  # pragma: no cover\n'
        '+    return 0\n'
    )
    problems, _, _ = check_patch('gen', tmp_path, detail, diff)
    assert problems == ['coverage: gen patch: gen/r2s_gen/m.py:3: excluded without a reason']


def test_gcovr_reasoned_needs_a_reason_on_the_start_line():
    source = [
        '// GCOV_EXCL_BR_START: installed fields always match',
        'if (x < 0) {',
        '  return -1;',
        '}',
        '// GCOV_EXCL_BR_STOP',
        '// GCOV_EXCL_START',
        'if (y) {',
        '}',
        '// GCOV_EXCL_STOP',
        'return -1;  // GCOV_EXCL_LINE: callers clamp',
        'return -2;  // GCOV_EXCL_LINE',
    ]
    assert coverage_gate.gcovr_reasoned(source) == {2, 3, 4, 10}


def test_gcovr_reasoned_accepts_a_dash_separator():
    source = ['return -1;  // GCOV_EXCL_BR_LINE - only a hand-built layout misses it']
    assert coverage_gate.gcovr_reasoned(source) == {1}


def test_coveragepy_reasoned_accepts_dash_and_colon_but_not_a_bare_marker():
    source = [
        'return 1  # pragma: no cover - callers clamp',
        'return 2  # pragma: no cover: every suffix returns above',
        'return 3  # pragma: no cover',
        'return 4',
    ]
    assert coverage_gate.coveragepy_reasoned(source) == {1, 2}


def gate_setup(tmp_path):
    write_source(tmp_path, 'core/src/f.c', 'int f(void) {\n  return 1;\n}\n')
    head = write_summary(tmp_path / 'head.json', 95, 100, 90, 100)
    base = write_summary(tmp_path / 'base.json', 96, 100, 92, 100)
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {'core/src/f.c': [(1, 2, False, []), (2, 2, False, [])]},
    )
    diff = (
        'diff --git a/core/src/f.c b/core/src/f.c\n'
        'new file mode 100644\n'
        '--- /dev/null\n'
        '+++ b/core/src/f.c\n'
        '@@ -0,0 +1,3 @@\n'
        '+int f(void) {\n'
        '+  return 1;\n'
        '+}\n'
    )
    return head, base, detail, diff


def test_gate_files_passes_a_covered_patch_despite_a_dip(tmp_path):
    head, base, detail, diff = gate_setup(tmp_path)
    problems, advisories, reports = coverage_gate.gate_files(
        'core', head, base, detail, diff, str(tmp_path), {'core/src/f.c'}
    )
    assert (problems, advisories) == ([], [])
    assert any('patch lines 2/2 covered' in line for line in reports)


def test_gate_files_names_an_uncovered_changed_line(tmp_path):
    head, base, _, diff = gate_setup(tmp_path)
    detail = write_gcovr_detail(
        tmp_path / 'detail.json',
        {'core/src/f.c': [(1, 2, False, []), (2, 0, False, [])]},
    )
    problems, _, _ = coverage_gate.gate_files(
        'core', head, base, detail, diff, str(tmp_path), {'core/src/f.c'}
    )
    assert problems == ['coverage: core patch: core/src/f.c:2: uncovered line']


def test_gate_files_skips_the_patch_without_a_diff(tmp_path):
    head, _, _, _ = gate_setup(tmp_path)
    problems, advisories, reports = coverage_gate.gate_files(
        'core', head, None, None, None, None, set()
    )
    assert (problems, advisories) == ([], [])
    assert len(reports) == 1 and 'line 95.00%' in reports[0]


def test_gate_files_still_fails_below_the_floor(tmp_path):
    low = write_summary(tmp_path / 'low.json', 80, 100, 70, 100)
    problems, _, _ = coverage_gate.gate_files('core', low, None, None, None, None, set())
    assert any('floor' in p for p in problems)


def test_main_runs_the_patch_and_names_the_file_and_line(tmp_path, capsys):
    head, base, _, diff = gate_setup(tmp_path)
    bad_detail = write_gcovr_detail(
        tmp_path / 'bad-detail.json',
        {'core/src/f.c': [(1, 2, False, []), (2, 0, False, [])]},
    )
    diff_path = tmp_path / 'patch.diff'
    diff_path.write_text(diff)
    argv = [
        '--component',
        'core',
        '--head',
        head,
        '--base',
        base,
        '--detail',
        bad_detail,
        '--diff',
        str(diff_path),
        '--repo',
        str(tmp_path),
        '--measured',
        'core/src/f.c',
    ]
    assert coverage_gate.main(argv) == 1
    assert 'coverage: core patch: core/src/f.c:2: uncovered line' in capsys.readouterr().out


def test_read_detail_routes_gen_to_the_coveragepy_shape(tmp_path):
    detail = write_coveragepy(
        tmp_path / 'detail.json',
        {'gen/r2s_gen/m.py': ([1], [2], [3], [], [])},
    )
    parsed = coverage_gate.read_detail('gen', detail)
    first = parsed['gen/r2s_gen/m.py'][1]
    second = parsed['gen/r2s_gen/m.py'][2]
    third = parsed['gen/r2s_gen/m.py'][3]
    assert (first.count, first.excluded) == (1, False)
    assert (second.count, second.excluded) == (0, False)
    assert (third.count, third.excluded) == (0, True)
    assert first.branches == []
