# The only entry point. This is the minimal version from #4; #7 adds the
# Docker image and the remaining targets listed in AGENTS.md.

.PHONY: help test

help: ## List all targets
	@grep -E '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-12s %s\n", $$1, $$2}'

test: ## Run scripts/ci-test.sh, the exact suite CI runs (scope with LINT=1 SCRIPTS=1 ...)
	@bash scripts/ci-test.sh
