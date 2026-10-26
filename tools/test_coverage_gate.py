import json

import coverage_gate


def write_summary(path, line_covered, line_total, branch_covered, branch_total):
    payload = {
        "line_covered": line_covered,
        "line_total": line_total,
        "branch_covered": branch_covered,
        "branch_total": branch_total,
    }
    path.write_text(json.dumps(payload))
    return str(path)


def errors(component, head, base, advisory=frozenset()):
    problems, _ = coverage_gate.check_files(component, head, base, advisory)
    return problems


def test_equal_figures_pass(tmp_path):
    head = write_summary(tmp_path / "head.json", 96, 100, 90, 100)
    base = write_summary(tmp_path / "base.json", 96, 100, 90, 100)
    assert coverage_gate.check_files("core", head, base) == ([], [])


def test_a_lowered_line_figure_fails(tmp_path):
    head = write_summary(tmp_path / "head.json", 95, 100, 90, 100)
    base = write_summary(tmp_path / "base.json", 96, 100, 90, 100)
    problems = errors("core", head, base)
    assert len(problems) == 1 and "line" in problems[0]


def test_a_lowered_branch_figure_fails(tmp_path):
    head = write_summary(tmp_path / "head.json", 96, 100, 91, 100)
    base = write_summary(tmp_path / "base.json", 96, 100, 92, 100)
    problems = errors("core", head, base)
    assert len(problems) == 1 and "branch" in problems[0]


def test_a_raised_figure_passes(tmp_path):
    head = write_summary(tmp_path / "head.json", 97, 100, 91, 100)
    base = write_summary(tmp_path / "base.json", 96, 100, 90, 100)
    assert coverage_gate.check_files("core", head, base) == ([], [])


def test_below_the_floor_fails_even_above_base(tmp_path):
    head = write_summary(tmp_path / "head.json", 94, 100, 90, 100)
    base = write_summary(tmp_path / "base.json", 93, 100, 90, 100)
    assert any("floor" in p for p in errors("core", head, base))


def test_an_advisory_floor_is_reported_without_failing(tmp_path):
    head = write_summary(tmp_path / "head.json", 94, 100, 90, 100)
    base = write_summary(tmp_path / "base.json", 93, 100, 90, 100)
    problems, advisories = coverage_gate.check_files("core", head, base, {"core"})
    assert problems == [] and any("floor" in a for a in advisories)


def test_a_drop_fails_even_with_an_advisory_floor(tmp_path):
    head = write_summary(tmp_path / "head.json", 94, 100, 89, 100)
    base = write_summary(tmp_path / "base.json", 95, 100, 90, 100)
    problems, _ = coverage_gate.check_files("core", head, base, {"core"})
    assert any("base" in p for p in problems)


def test_no_base_checks_only_the_floor(tmp_path):
    head = write_summary(tmp_path / "head.json", 96, 100, 90, 100)
    assert coverage_gate.check_files("core", head, None) == ([], [])


def test_an_unknown_component_is_a_problem(tmp_path):
    head = write_summary(tmp_path / "head.json", 96, 100, 90, 100)
    problems, _ = coverage_gate.check_files("gen", head, None)
    assert problems != []


def test_an_empty_measurement_is_a_problem(tmp_path):
    head = write_summary(tmp_path / "head.json", 0, 0, 0, 0)
    assert errors("core", head, None) == ["coverage: core measured no lines"]
    full = write_summary(tmp_path / "full.json", 96, 100, 90, 100)
    assert errors("core", full, head) == ["coverage: core measured no lines on base"]


def test_main_reports_one_line_per_failure(tmp_path, capsys):
    head = write_summary(tmp_path / "head.json", 95, 100, 91, 100)
    base = write_summary(tmp_path / "base.json", 96, 100, 92, 100)
    assert coverage_gate.main(["--component", "core", "--head", head, "--base", base]) == 1
    assert len(capsys.readouterr().out.strip().splitlines()) == 2


def test_main_passes_on_an_advisory_floor_breach(tmp_path, capsys):
    head = write_summary(tmp_path / "head.json", 71, 100, 51, 100)
    base = write_summary(tmp_path / "base.json", 71, 100, 51, 100)
    argv = ["--component", "bridge", "--head", head, "--base", base, "--advisory-floor", "bridge"]
    assert coverage_gate.main(argv) == 0
    assert "(advisory)" in capsys.readouterr().out
