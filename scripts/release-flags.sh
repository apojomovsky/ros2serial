#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# Print the one gh release create flag for a tag name: a plain version tag
# publishes as a full release marked Latest, while a tag with a SemVer
# pre-release suffix (a dash after the version, as in v0.2.0-rc.1) publishes
# as a pre-release. Usage: release-flags.sh <tag>
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: release-flags.sh <tag>" >&2; exit 2; }
case $1 in
  *"-"*) echo --prerelease ;;
  *) echo --latest ;;
esac
