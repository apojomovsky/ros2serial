#!/usr/bin/env bash
# Python lint gate: ruff check and ruff format, the pinned version from the
# image (python3 -m keeps it ahead of any other ruff on PATH).
set -uo pipefail
if ! python3 -m ruff --version >/dev/null 2>&1; then
  echo "ruff not found: install the versions pinned in ci/versions.env"
  exit 1
fi
python3 -m ruff check . && python3 -m ruff format --check .
