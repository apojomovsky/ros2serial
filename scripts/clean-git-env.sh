# Sourced, not run: drops the repository-local git variables (GIT_DIR,
# GIT_INDEX_FILE and the like) that git exports to hooks. Without this, a test
# that builds a throwaway repo would run its git commands on the real one.
while read -r var; do unset "$var"; done < <(git rev-parse --local-env-vars 2>/dev/null)
