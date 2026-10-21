#!/usr/bin/env bash
# Report what first-time setup is missing; change nothing. Exits 1 if a
# required host dependency is missing.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
missing=0
for tool in git make docker; do
  if command -v "$tool" >/dev/null 2>&1; then
    echo "ok       $tool"
  else
    echo "MISSING  $tool"
    missing=1
  fi
done
if command -v docker >/dev/null 2>&1; then
  if docker info >/dev/null 2>&1; then echo "ok       docker daemon reachable"; else echo "MISSING  docker daemon (is it running? can this user reach it?)"; missing=1; fi
  tag=$(bash "$root/scripts/dev-image-tag.sh")
  if docker image inspect "$tag" >/dev/null 2>&1; then echo "ok       dev image $tag"; else echo "todo     dev image $tag not built: make image"; fi
fi
if [ "$(git -C "$root" config core.hooksPath 2>/dev/null)" = ".githooks" ]; then
  echo "ok       git hooks installed"
else
  echo "todo     git hooks not installed: make setup-hooks"
fi
exit $missing
