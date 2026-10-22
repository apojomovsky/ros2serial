#!/usr/bin/env bash
# The takeoff ritual (AGENTS.md): run before opening a PR, from the PR's worktree.
# Prints one FIX line per blocking problem and exits 1 while any remain.
#   BASE     ref the PR targets (default origin/master; a stacked PR uses its parent's branch)
#   PR_BODY  file holding the PR body, to check it too
#   TEST=1   also run the full suite
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
  fix "branch is behind $base: merge $base into it"
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

# 11. Hooks installed.
[ "$(git config core.hooksPath)" = .githooks ] || fix "git hooks are not installed: make setup-hooks"

# 12. README and docs freshness (advisory).
if grep -qE '^(core/include/|cpp/|gen/r2s_gen/cli.py|bridge/r2s_bridge/config/)' <<<"$changed" \
  && ! grep -qE '^(README.md|docs/)' <<<"$changed" \
  && ! { [ -n "${PR_BODY:-}" ] && grep -q 'README: n/a' "$PR_BODY" 2>/dev/null; }; then
  note "user-visible files changed without README.md or docs/: update them or write README: n/a in the PR body"
fi

# 13. The full suite.
if [ "${TEST:-}" = 1 ]; then
  bash "$root/scripts/ci-test.sh" >/dev/null 2>&1 || {
    bash "$root/scripts/ci-test.sh" 2>&1 | tail -20
    fix "the suite fails: bash scripts/ci-test.sh"
  }
fi

for n in "${notes[@]}"; do echo "note: $n"; done
if [ "${#fixes[@]}" -gt 0 ]; then
  for f in "${fixes[@]}"; do echo "FIX: $f"; done
  exit 1
fi
echo "takeoff: all clear"
