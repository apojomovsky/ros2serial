#!/usr/bin/env bash
# The budgets gate: build the examples, measure flash, RAM and worst-case
# stack, and judge the head numbers against tools/budgets.yaml. Both checkers
# also run against a deliberately breached fixture that must fail, so the gate
# proves it can catch a breach on every run. With --base REF the head numbers
# are judged by that ref's script and budgets as well, and the table compares
# head with base. With --report the tables print and nothing is judged.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/examples

base_ref="${BASE:-}"
report_only=0
for arg in "$@"; do
  case $arg in
  --base=*) base_ref=${arg#--base=} ;;
  --report) report_only=1 ;;
  *)
    echo "budgets-check: unknown argument $arg" >&2
    exit 2
    ;;
  esac
done

status=0
export R2S_STACK_BUILD=1
bash "$root/scripts/build-examples.sh" || status=1
if [ "$status" -ne 0 ]; then
  echo "budgets: the example builds failed" >&2
  exit 1
fi

# The reports every later step reads.
python3 "$root/tools/size_report.py" measure --build-dir "$build" --out "$build/measurements.json" || status=1
python3 "$root/tools/stack_report.py" collect --build-dir "$build" --out "$build/stack.json" || status=1
if [ "$status" -ne 0 ]; then
  echo "budgets: measuring the firmware failed" >&2
  exit 1
fi

budgets="$root/tools/budgets.yaml"
breach="$root/tools/budgets_breach_fixture.yaml"
base_measurements=""
base_stack=""

# The base half: the same run judges the head numbers by the base ref's
# script and budgets, so a pull request cannot relax its own judge. A base
# from before the budgets land has no script, and a base that is HEAD (a
# push to the base branch itself) has nothing to compare: either way only
# the head judge runs.
if [ -n "$base_ref" ] && [ "$(git rev-parse HEAD)" = "$(git rev-parse "$base_ref" 2>/dev/null)" ]; then
  echo "budgets: base is HEAD; only the head judge runs" >&2
  base_ref=""
fi
if [ -n "$base_ref" ]; then
  if git cat-file -e "$base_ref:tools/size_report.py" 2>/dev/null &&
    git cat-file -e "$base_ref:tools/stack_report.py" 2>/dev/null &&
    git cat-file -e "$base_ref:tools/budgets.yaml" 2>/dev/null; then
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/r2s-budgets.XXXXXX")
    # shellcheck disable=SC2064 # the worktree path is fixed when the trap is set.
    trap "git worktree remove --force '$tmp/base' >/dev/null 2>&1; rm -rf '$tmp'" EXIT
    mkdir -p "$tmp/btools"
    git show "$base_ref:tools/size_report.py" >"$tmp/btools/size_report.py"
    git show "$base_ref:tools/stack_report.py" >"$tmp/btools/stack_report.py"
    git show "$base_ref:tools/budgets.yaml" >"$tmp/btools/budgets.yaml"
    git worktree add --detach "$tmp/base" "$base_ref" >/dev/null 2>&1 || status=1
    if [ "$status" -eq 0 ]; then
      R2S_BUILD_DIR="$tmp/base-build" bash "$tmp/base/scripts/build-examples.sh" || status=1
    fi
    if [ "$status" -eq 0 ]; then
      PYTHONPATH="$tmp/btools" python3 "$tmp/btools/size_report.py" measure \
        --build-dir "$tmp/base-build/examples" --out "$tmp/base-measurements.json" || status=1
      PYTHONPATH="$tmp/btools" python3 "$tmp/btools/stack_report.py" collect \
        --build-dir "$tmp/base-build/examples" --out "$tmp/base-stack.json" || status=1
    fi
    if [ "$status" -ne 0 ]; then
      echo "budgets: measuring the base firmware failed" >&2
      exit 1
    fi
    base_measurements="$tmp/base-measurements.json"
    base_stack="$tmp/base-stack.json"
  else
    echo "budgets: $base_ref carries no budget tools; only the head judge runs" >&2
  fi
fi

if [ "$report_only" = 1 ]; then
  python3 "$root/tools/size_report.py" check --measurements "$build/measurements.json" \
    --budgets "$budgets" --markdown /dev/stdout || true
  python3 "$root/tools/stack_report.py" check --stack "$build/stack.json" \
    --budgets "$budgets" --markdown /dev/stdout || true
  exit 0
fi

base_size_args=()
base_stack_args=()
if [ -n "$base_measurements" ]; then
  base_size_args=(--base "$base_measurements")
  base_stack_args=(--base "$base_stack")
fi
python3 "$root/tools/size_report.py" check --measurements "$build/measurements.json" \
  --budgets "$budgets" "${base_size_args[@]}" --markdown "$build/size-table.md" || status=1
python3 "$root/tools/stack_report.py" check --stack "$build/stack.json" \
  --budgets "$budgets" "${base_stack_args[@]}" --markdown "$build/stack-table.md" || status=1

# The gate must fail: a breached fixture proves both checkers catch a breach.
if python3 "$root/tools/size_report.py" check --measurements "$build/measurements.json" \
  --budgets "$breach" >/dev/null 2>&1; then
  echo "budgets: the breached fixture passed the size check" >&2
  status=1
fi
if python3 "$root/tools/stack_report.py" check --stack "$build/stack.json" \
  --budgets "$breach" >/dev/null 2>&1; then
  echo "budgets: the breached fixture passed the stack check" >&2
  status=1
fi

{
  echo "### Flash and RAM"
  echo
  cat "$build/size-table.md"
  echo "### Worst-case stack"
  echo
  cat "$build/stack-table.md"
} >"$build/budgets-table.md"
echo "TABLE $build/budgets-table.md"

if [ -n "$base_measurements" ]; then
  PYTHONPATH="$tmp/btools" python3 "$tmp/btools/size_report.py" check \
    --measurements "$build/measurements.json" --budgets "$tmp/btools/budgets.yaml" || status=1
  PYTHONPATH="$tmp/btools" python3 "$tmp/btools/stack_report.py" check \
    --stack "$build/stack.json" --budgets "$tmp/btools/budgets.yaml" || status=1
fi

exit "$status"
