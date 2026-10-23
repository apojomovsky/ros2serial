import inspect

import next_ticket


def issue(number, labels=(), body="", state="open", title=None):
    return {
        "number": number,
        "title": title or f"ticket {number}",
        "state": state,
        "labels": [{"name": n} for n in labels],
        "body": body,
    }


def pr(number, body, labels=(), created="2026-10-21T00:00:00Z"):
    return {"number": number, "body": body, "labels": [{"name": n} for n in labels], "created_at": created}


ISSUES = [
    issue(1, ["area:ci"], state="closed"),
    issue(2, ["area:core"], body="Depends on #3"),
    issue(3, ["area:spec"]),
    issue(4, ["area:core"]),
    issue(5, ["area:gen"], body="Depends on #1"),
    issue(6, ["area:spec"]),
    issue(7, ["area:ports", "needs-hardware"]),
    issue(8, ["area:bridge", "dispatch-only"]),
    issue(9, ["area:bridge"]),
]
PRS = [pr(20, "Closes #4\n\nagent: x@y"), pr(21, "Closes #3")]


def test_only_unblocked_unclaimed_non_conflicting_tickets_are_offered():
    report = next_ticket.select(ISSUES, PRS)
    assert [t["number"] for t in report.available] == [5, 9]


def test_every_skipped_ticket_has_a_reason():
    reasons = next_ticket.select(ISSUES, PRS).skipped
    assert "#3" in reasons[2] and "open" in reasons[2]
    assert "PR #21" in reasons[3]
    assert "PR #20" in reasons[4]
    assert "area:spec" in reasons[6]
    assert "hardware" in reasons[7]
    assert "maintainer" in reasons[8]


def test_a_pr_without_labels_takes_its_areas_from_the_issues_it_closes():
    report = next_ticket.select([issue(4, ["area:core"]), issue(10, ["area:core"])], [pr(20, "Closes #4")])
    assert report.available == []
    assert "area:core" in report.skipped[10]


def test_pr_labels_count_too():
    report = next_ticket.select([issue(10, ["area:gen"])], [pr(22, "No issue", labels=["area:gen"])])
    assert report.available == []


def test_two_prs_claiming_one_issue_is_a_race_and_the_older_wins():
    prs = [pr(31, "Closes #3", created="2026-10-21T02:00:00Z"), pr(30, "Fixes #3", created="2026-10-21T01:00:00Z")]
    report = next_ticket.select([issue(3, ["area:spec"])], prs)
    assert report.races == {3: [30, 31]}


def test_a_dependency_missing_from_the_list_counts_as_open():
    report = next_ticket.select([issue(2, ["area:core"], body="Depends on #99")], [])
    assert report.available == []
    assert "#99" in report.skipped[2]


def test_an_agents_own_open_pr_blocks_its_areas_like_anyones():
    """Tickets are not stacked, so an agent's own PR holds its area too."""
    issues = [issue(6, ["area:ci"]), issue(7, ["area:ci"])]
    prs = [pr(16, "Closes #6\n\nagent: claude@box")]
    assert next_ticket.select(issues, prs).available == []
    # Nothing exempts an agent any more, so nothing takes its ID.
    assert list(inspect.signature(next_ticket.select).parameters) == ["issues", "prs"]


def test_dependency_lines_are_parsed_in_every_common_form():
    open3 = issue(3, ["area:spec"])
    for body in [
        "depends on #3",
        "- Depends on #3",
        "Depends on: #3",
        "**Depends on** #3",
        "Depends on #1 and #3",
        "Depends on #4, #3",
    ]:
        report = next_ticket.select(
            [issue(1, state="closed"), issue(4, state="closed"), issue(2, ["area:core"], body=body), open3], []
        )
        assert 2 in report.skipped and "#3" in report.skipped[2], (body, report.skipped)


def test_issues_that_are_pull_requests_are_not_tickets():
    item = issue(40, ["area:ci"])
    item["pull_request"] = {"url": "x"}
    assert next_ticket.select([item], []).available == []


def test_a_dependency_on_a_merged_pr_counts_as_done():
    merged = issue(19, state="closed")
    merged["pull_request"] = {"url": "x"}
    report = next_ticket.select([merged, issue(2, ["area:core"], body="Depends on #19")], [])
    assert [t["number"] for t in report.available] == [2]


def test_claims_are_case_insensitive_and_accept_issue_urls():
    issues = [issue(3, ["area:spec"]), issue(5, ["area:gen"])]
    prs = [pr(20, "closes #3"), pr(21, "Closes https://github.com/apojomovsky/ros2serial/issues/5")]
    report = next_ticket.select(issues, prs)
    assert report.available == [] and "PR #20" in report.skipped[3] and "PR #21" in report.skipped[5]


def test_closing_the_same_issue_twice_in_one_body_is_not_a_race():
    assert next_ticket.select([issue(3)], [pr(20, "Closes #3\n\nCloses #3")]).races == {}


def test_main_prints_races_and_available_tickets(tmp_path, capsys):
    import json

    (tmp_path / "i.json").write_text(json.dumps([issue(3, ["area:spec"]), issue(9, ["area:bridge"])]))
    (tmp_path / "p.json").write_text(json.dumps([pr(30, "Closes #3"), pr(31, "Closes #3")]))
    import sys

    sys.argv = ["next_ticket.py", "--from-json", str(tmp_path / "i.json"), str(tmp_path / "p.json")]
    assert next_ticket.main() == 0
    out = capsys.readouterr().out
    assert "race on #3" in out and "#9 ticket 9" in out
