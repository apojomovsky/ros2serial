#!/usr/bin/env bash
# The takeoff ritual (AGENTS.md): run before opening a PR, from the PR's worktree.
# Prints one FIX line per blocking problem and exits 1 while any remain.
#   BASE     ref the PR targets (default origin/master; a PR is never stacked)
#   PR_BODY  file holding the PR body, to check it too
#   PR_TITLE the title the PR will carry, to check it against the commits
#   TEST=1   also run the full suite
#   R2S_SUITE  script to run instead of the suite, so a test can see what the
#              suite was given
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
base=${BASE:-origin/master}
fixes=()
notes=()
fix() { fixes+=("$1"); }
note() { notes+=("$1"); }

# 1. Clean tree, not behind the base.
[ -z "$(git status --porcelain)" ] || fix "working tree is not clean: commit or stash first"
if ! git rev-parse --verify -q "$base" >/dev/null; then
  fix "base $base not found: fetch it, or set BASE"
elif ! git merge-base --is-ancestor "$base" HEAD; then
  # Merge the base in: the PR keeps its history and the squash drops the merge
  # commit, so catching up needs no force push.
  fix "branch is behind $base: merge $base into it (git merge $base) and push"
fi

# 2. In a worktree, on a ticket branch.
git_dir=$(git rev-parse --absolute-git-dir)
common=$(cd "$(git rev-parse --git-common-dir)" && pwd)
top=$(git rev-parse --show-toplevel)
if [ "$git_dir" = "$common" ] || [[ "$top" != */.worktrees/* ]]; then
  fix "not in a .worktrees/ worktree: create one per ticket (AGENTS.md, Worktrees)"
fi
[ "$(git branch --show-current)" = master ] && fix "on master: work on a <type>/<issue>-<slug> branch"

changed=$(git diff --name-only "$base...HEAD" 2>/dev/null)

# 3. No plan files.
while read -r f; do
  [ -n "$f" ] && fix "plan file in the diff: $f (distill decisions into an ADR and remove it)"
done < <(grep '^docs/plans/' <<<"$changed")

# 4. Commit hygiene and whitespace.
while read -r line; do
  [ -n "$line" ] && fix "commit $line"
done < <(python3 "$root/scripts/commit_check.py" --range "$base..HEAD" 2>&1)
git diff --check "$base...HEAD" >/dev/null 2>&1 || fix "whitespace errors in the diff: git diff --check $base...HEAD"
# The title is the subject that lands, so it must carry what the commits
# declare. The check is the one CI runs, so the problem shows here.
if [ -n "${PR_TITLE:-}" ]; then
  if git rev-parse --verify -q "$base" >/dev/null; then
    while read -r line; do
      [ -n "$line" ] && fix "PR title: ${line#PR title: }"
    done < <(python3 "$root/scripts/commit_check.py" --title "$PR_TITLE" --covers-range "$base..HEAD" 2>&1)
  else
    while read -r line; do
      [ -n "$line" ] && fix "PR title: ${line#PR title: }"
    done < <(python3 "$root/scripts/commit_check.py" --title "$PR_TITLE" 2>&1)
  fi
else
  note "PR title: not checked, pass PR_TITLE=\"<the title the PR will carry>\""
fi

# 5. Compiler warnings.
bash "$root/scripts/check-warnings.sh" >/dev/null 2>&1 \
  || fix "compiler warnings or cppcheck findings: run make check-warnings"

# 6. Lint.
bash "$root/scripts/lint-python.sh" >/dev/null 2>&1 || fix "Python lint fails: bash scripts/lint-python.sh"
bash "$root/scripts/c-format.sh" >/dev/null 2>&1 || fix "C formatting differs: make fmt"

# 7. Zone check.
if [ -f "$root/tools/zone_check.py" ]; then
  python3 "$root/tools/zone_check.py" >/dev/null 2>&1 || fix "zone check fails: python3 tools/zone_check.py"
else
  note "zone check: arrives with #9"
fi

# 8. Spec and vectors change together.
spec=$(grep -c '^spec/protocol.md$' <<<"$changed")
vectors=$(grep -c '^spec/vectors/' <<<"$changed")
[ "$spec" -gt 0 ] && [ "$vectors" -eq 0 ] && fix "spec/protocol.md changed without spec/vectors changes"
[ "$vectors" -gt 0 ] && [ "$spec" -eq 0 ] && fix "spec/vectors changed without spec/protocol.md changes"

# 9. Prose.
while read -r line; do
  [ -n "$line" ] && fix "prose: $line"
done < <(python3 "$root/scripts/prose_check.py" --verify --base "$base" 2>&1)

# 10. PR body.
if [ -n "${PR_BODY:-}" ]; then
  if [ ! -f "$PR_BODY" ]; then
    fix "PR_BODY file $PR_BODY not found"
  else
    while read -r line; do
      [ -n "$line" ] && fix "$line"
    done < <(python3 "$root/scripts/pr_body_check.py" --full --file "$PR_BODY" 2>&1 || echo "PR body check failed to run")
  fi
else
  note "PR body: pass PR_BODY=<file> to check it"
fi

# 11. The repository's merge settings.
# A rule in a document says what should happen; the settings say what may
# happen, so a merge commit is only possible if the settings allow it. The
# check needs gh, and a host without it says so rather than passing.
settings_said=$(bash "$root/scripts/merge-settings.sh" 2>&1)
case $? in
  0) ;;
  75) note "the repository's merge settings are not checked (${settings_said#merge-settings: })" ;;
  *) fix "the repository's merge settings do not match ADR-003: $settings_said" ;;
esac

# 12. Hooks installed.
[ "$(git config core.hooksPath)" = .githooks ] || fix "git hooks are not installed: make setup-hooks"

# 13. README and docs freshness (advisory).
if grep -qE '^(core/include/|cpp/|gen/r2s_gen/cli.py|bridge/r2s_bridge/config/)' <<<"$changed" \
  && ! grep -qE '^(README.md|docs/)' <<<"$changed" \
  && ! { [ -n "${PR_BODY:-}" ] && grep -q 'README: n/a' "$PR_BODY" 2>/dev/null; }; then
  note "user-visible files changed without README.md or docs/: update them or write README: n/a in the PR body"
fi

# 14. The full suite.
if [ "${TEST:-}" = 1 ]; then
  # The suite's own tests call this ritual, and a ritual that sees TEST=1
  # runs the suite again, so TEST does not go down with it. The scopes do:
  # the child must do what this run was asked to do, no more. This ritual's
  # own inputs go too, so a nested ritual reads its own base and body
  # rather than the outer ones. R2S_SUITE names the script to run, so a
  # test can see what the suite was given.
  suite=${R2S_SUITE:-$root/scripts/ci-test.sh}
  # The output is kept, so a failure can be shown without running the suite
  # again: a second run would be refused by the lock the first one holds.
  suite_log=$(mktemp "${TMPDIR:-/tmp}/r2s-suite.XXXXXX")
  if ! env -u TEST -u BASE -u PR_BODY bash "$suite" >"$suite_log" 2>&1; then
    # The failed sections are the point, so they are shown. The suite names
    # them on its last line; a section that is not named falls back to the
    # last lines of the log.
    shown=0
    for s in $(sed -n 's/^failed sections: //p' "$suite_log"); do
      for name in $s; do
        sed -n "/^== $name$/,/^== $name:/p" "$suite_log"
        shown=1
      done
    done
    [ "$shown" = 1 ] || tail -5 "$suite_log"
    rm -f "$suite_log"
    fix "the suite fails: bash scripts/ci-test.sh"
  else
    rm -f "$suite_log"
  fi
fi

for n in "${notes[@]}"; do echo "note: $n"; done
if [ "${#fixes[@]}" -gt 0 ]; then
  for f in "${fixes[@]}"; do echo "FIX: $f"; done
  exit 1
fi
echo "takeoff: all clear"
