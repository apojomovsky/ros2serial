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
