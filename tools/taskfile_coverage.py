"""Every tracked file maps to a task or says why not.

The retired scope map failed safe to the full suite on unknown paths; the
Taskfile cannot, so this check fails on a tracked file no task lists. A
file no section reads carries its reason in UNLISTED instead, so adding a
file the suite should cover means listing it in the Taskfile, and wiring a
new reader means the same.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Tracked files no section reads, each with the reason nothing lists it.
UNLISTED = {
    # Tag, release-cut and scheduled flows run ungated, and the PR-body and
    # PR-title flows run on every pull request event without a plan.
    ".github/workflows/release.yml": "tag-only flow, runs ungated",
    ".github/workflows/cut-release.yml": "manual release cut, runs ungated",
    ".github/workflows/pr-body.yml": "runs on every PR event, ungated",
    ".github/workflows/pr-title.yml": "runs on every PR event, ungated",
    ".github/workflows/nightly.yml": "schedule and dispatch only, runs ungated",
    # The image build plumbing: the image content comes from the Dockerfile
    # and ci/versions.env, which every task lists.
    ".github/actions/dev-image/action.yml": "image build plumbing, content is Dockerfile plus versions.env",
    # Ports no suite section builds yet. Wiring one into a section lists it
    # in that section's task.
    "ports/avr": "no suite section builds this port yet",
    "ports/ch32v": "no suite section builds this port yet",
    "ports/epic-hal": "no suite section builds this port yet",
    "ports/stm32": "no suite section builds this port yet",
    # Read by the standing lint gate, which always runs, so no task lists them.
    ".clang-format": "read by the standing lint gate, which always runs",
    "LICENSE": "content pinned by the standing lint gate, which always runs",
    # Read by git at checkout; no section parses it.
    ".gitattributes": "read by git at checkout, no section parses it",
}


def glob_match(pattern, path):
    out = []
    i = 0
    while i < len(pattern):
        char = pattern[i]
        if char == "*":
            if pattern[i : i + 3] == "**/":
                out.append("(.*/)?")
                i += 3
            elif pattern[i : i + 2] == "**":
                out.append(".*")
                i += 2
            else:
                out.append("[^/]*")
                i += 1
        elif char == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(char))
            i += 1
    return re.fullmatch("".join(out), path) is not None


def task_sources(taskfile):
    import yaml

    tasks = yaml.safe_load((taskfile).read_text())["tasks"]
    by_task = {}
    for name, task in tasks.items():
        if name == "default":
            continue
        by_task[name] = task.get("sources", []) or []
    return by_task


def task_covers(sources, path):
    hit = any(isinstance(s, str) and glob_match(s, path) for s in sources)
    if not hit:
        return False
    return not any(isinstance(s, dict) and glob_match(s.get("exclude", ""), path) for s in sources)


def covered(by_task, path):
    return any(task_covers(sources, path) for sources in by_task.values())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(ROOT))
    args = parser.parse_args(argv)
    root = Path(args.root)
    proc = subprocess.run(
        ["git", "ls-files"],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        print(f"taskfile-coverage: git ls-files failed: {proc.stderr.strip()}")
        return 1
    by_task = task_sources(root / "Taskfile.yml")
    missing = []
    for path in proc.stdout.splitlines():
        if not path or covered(by_task, path):
            continue
        if any(path == listed or path.startswith(listed.rstrip("/") + "/") for listed in UNLISTED):
            continue
        missing.append(path)
    if missing:
        for path in missing:
            print(f"taskfile-coverage: no task reads {path}")
        return 1
    print("taskfile-coverage: every tracked file maps to a task")
    return 0


if __name__ == "__main__":
    sys.exit(main())
