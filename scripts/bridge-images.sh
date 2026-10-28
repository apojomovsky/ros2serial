#!/usr/bin/env bash
# Print the bridge job's matrix and images as GITHUB_OUTPUT lines, read from
# ci/versions.env. The job's container image is fixed before any step runs, so
# a job of its own resolves the pins first.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
file=$root/ci/versions.env

fail() {
  echo "$*" >&2
  exit 1
}

distros=()
images=()
# read fails on a last line without a trailing newline, so the guard keeps it.
while IFS= read -r line || [ -n "$line" ]; do
  [[ $line =~ ^ROS_([A-Z0-9]+)_IMAGE=(.*)$ ]] || continue
  key=${BASH_REMATCH[1]}
  distro=$(printf '%s' "$key" | tr '[:upper:]' '[:lower:]')
  image=${BASH_REMATCH[2]}
  [[ $image =~ ^ros:$distro-[a-z0-9-]+@sha256:[0-9a-f]{64}$ ]] ||
    fail "$file: ROS_${key}_IMAGE is not a digest-pinned image for $distro: '${image}'"
  for other in "${!distros[@]}"; do
    if [ "${distros[other]}" = "$distro" ]; then
      fail "$file: ROS_${key}_IMAGE is pinned more than once"
    fi
  done
  distros+=("$distro")
  images+=("$image")
done <"$file"

[ ${#distros[@]} -gt 0 ] || fail "$file: no ROS_<distro>_IMAGE pins the bridge job's images"

join() {
  local IFS=,
  printf '%s' "$*"
}

matrix=()
pairs=()
for i in "${!distros[@]}"; do
  matrix+=("\"${distros[i]}\"")
  pairs+=("\"${distros[i]}\":\"${images[i]}\"")
done
printf 'matrix={"distro":[%s]}\n' "$(join "${matrix[@]}")"
printf 'images={%s}\n' "$(join "${pairs[@]}")"
