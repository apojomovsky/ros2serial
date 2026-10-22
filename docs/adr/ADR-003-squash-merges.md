# ADR-003: every pull request is squashed into one commit

**Status:** Accepted 2026-10-22

## Decision

- **Squash only.** A pull request lands on `master` as one commit. The repository settings allow a squash and forbid a merge commit and a rebase merge, so the method is chosen in the repository and not by whoever merges.
- **The landed commit is the PR title and nothing else.** The repository's squash title is `PR_TITLE` and its squash message is `BLANK`, so the landed commit's subject is the PR title followed by ` (#<PR number>)` and its body is empty. The PR title is therefore the only subject that reaches `master`, and it must carry the type and the `!` of a breaking change: `feat(core): ...`, `fix(bridge): ...`, `docs(...)`, `test(...)`, `ci(...)`, `build(...)`, `chore(...)`, `refactor(...)`, `perf(...)`, `revert(...)`, with `!` before the colon for a breaking change. A PR that holds a `feat` commit but is titled `docs(...)` does not reach the changelog or the version bump, and a `!` on a commit is lost with the commit, so the title must carry it. The commit check enforces this: the title must carry the strongest type its commits declare, and the `!` if any of them has one.
- **The PR's own commits are still checked.** The commit check and the commit-msg hook apply to every commit in the PR, because the squash is made from them. The three-line limit and the refusal of trailers apply to those commits. The landed commit carries no trailer, since the repository's squash message is blank.
- **A merge commit is not part of a PR's work.** The commit check's range skips merge commits, so a branch that carries one from before this rule is not failed over it. A branch behind its base is rebased, not merged.
- **One ticket at a time, from `master`.** A PR is not stacked on another PR's branch. A ticket starts from `master` and targets `master`. A squash drops the parent branch's commits, so a stacked PR's diff would carry its parent's commits again, and the parent is in `master` as one commit that the child cannot use.
- **The release subject is checked by a script.** `scripts/release-subject.sh` says whether a subject is the release commit for a version, and the tag job calls it. A squash appends the PR number to the subject, so the check matches the subject and the subject with the number, and the version is matched literally. The check is a script and not a line in the workflow, so the workflow and the test cannot drift apart.
- **Catching up with `master` needs the maintainer's go-ahead.** Every merge advances `master`, so an open branch falls behind it, and the only way to catch up is a rebase, which rewrites the pushed branch. The pre-push hook refuses every force push without the maintainer's go-ahead, and a rebase is a force push, so catching up means asking. That is a cost of the rule, not an oversight: the force-push gate exists so that work another agent may have pulled is never dropped, and the maintainer keeps it. An exemption for rebases would have been convenient and would have weakened the gate on its own.
- **The repository's settings are part of the rule.** Squash alone is allowed, a merge commit and a rebase merge are not, the squash title is the PR title, and the squash message is blank. These are the settings that make the landed commit a subject and nothing else, and they are what a change to the merge method would change.

## Rationale

The maintainer asked for one merge method and nothing else. Squash gives `master` one commit per PR, so a ticket's history is one line rather than the agent's sequence of working commits, and the release changelog is built from PR titles that were already checked.

Making the method a repository setting rather than a rule in a document is what makes it hold. A rule in `AGENTS.md` says what should happen; the setting says what may happen, and a merge commit cannot be created at all. The same is true of the squash message: left at its default, GitHub writes the PR's commit list and a `Co-authored-by:` trailer into the landed commit, which the three-line and no-trailer rules forbid. A blank message and the PR title make the landed commit a subject and nothing else.

Dropping stacking is the cost. A squash discards the parent branch's commits, so a child PR branched from it would show the parent's changes as its own diff. The alternative, rebasing the child onto the parent's squashed commit, puts a commit that is not the child's on the child's branch, which is the same problem in a different shape. One ticket at a time from `master` is the model that survives the merge method.

The release subject is a prefix match because the shape of a squash is GitHub's, not ours. Keeping the check in a script is what lets a test cover it: a check written only in a workflow cannot be tested without running the workflow.

## Rejected alternatives

- **Merge commits.** The maintainer does not want a method other than squash. The technical argument for them was that a stacked PR's commits stay identical; dropping stacking removes that argument.
- **Rebase merge.** It also produces one commit per PR, but it rewrites the branch's commits, so a branch that is pushed and then rebased cannot be pushed to without a force-push, and the repository's setting for it is disabled here.
- **An exemption for rebases in the force-push gate.** Every merge advances `master`, so an open branch falls behind it, and only a rebase catches it up, which is a force push. Allowing a rebase without the maintainer's go-ahead would have removed the gate for a class of pushes, and the gate is there so that work another agent may have pulled is never dropped. The cost of asking once per merge is the cost of the rule.
- **Squash with the default commit message.** GitHub writes the PR's commit subjects and bodies into the landed commit, plus a `Co-authored-by:` trailer. That is an eighty-line commit with a trailer, which the commit rules forbid, and it makes the changelog read as a list of working commits rather than of PRs.
- **Stacking, with a rebase onto the parent's squashed commit.** It keeps the stack, but the child's branch then carries a commit that is not the child's, and the child's diff against its base still shows the parent's work.
- **The subject check left in the workflow.** A test cannot reach a line in a workflow, so the check would be untested and would drift from the workflow the moment either changed.
