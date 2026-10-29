# The only entry point (AGENTS.md, Build and toolchain). Every target runs in
# the dev image through `docker run`, unless R2S_IN_IMAGE is set, which the
# image itself does: then the command runs directly. CI calls the same targets.

SHELL := /bin/bash
.DEFAULT_GOAL := help

IMAGE ?= $(shell bash scripts/dev-image-tag.sh)
BUILD_ARGS := $(shell grep -v '^\#' ci/versions.env | sed 's/^/--build-arg /')
# The main clone is mounted at its own absolute path, so git works inside the
# container from a worktree too (its .git file points into the main clone).
REPO_ROOT := $(abspath $(shell git rev-parse --git-common-dir 2>/dev/null)/..)
CACHE := $(HOME)/.cache/ros2serial
BUILD_DIR := $(CACHE)/build$(subst /,-,$(CURDIR))
# The image mounts the repository at its host path, so the suite can tell a
# tree another container also sees (this repository) from one only this
# container has (a test's temporary copy), whose path repeats in every
# container and must not key a shared lock.
export R2S_HOST_MOUNT := $(REPO_ROOT)
TTY := $(shell [ -t 0 ] && echo -it)
PASS_VARS := LINT SCRIPTS COMMITS CORE GEN BRIDGE CPP E2E COVERAGE DOCS EXAMPLES FUZZ PR_TITLE COMMIT_RANGE BASE PR_BODY TEST TARGET R2S_FUZZ_SECONDS R2S_FULL R2S_E2E_DOMAIN_ID R2S_ZENOH_ROUTER_PORT

DOCKER_BASE = docker run --rm $(TTY) --user $$(id -u):$$(id -g) \
	-v $(REPO_ROOT):$(REPO_ROOT) -w $(CURDIR) -v $(CACHE):$(CACHE) -e R2S_BUILD_DIR=$(BUILD_DIR) -e R2S_CACHE_DIR=$(CACHE) -e R2S_HOST_MOUNT=$(REPO_ROOT)
DOCKER_RUN = $(DOCKER_BASE) $(foreach v,$(PASS_VARS),-e $(v)) $(IMAGE)
in_image = $(if $(R2S_IN_IMAGE),$(1),$(DOCKER_RUN) $(1))
pending = echo "make $(1): not available until $(2)"; exit 1

.PHONY: help bootstrap doctor image shell exec test ci-local compile fuzz size-report vectors regen docs-gen \
	fmt lint check-warnings setup-hooks pre-pr-check guard-main-clone cache-dir

help: ## List all targets
	@grep -E '^[a-z][a-z-]*:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-16s %s\n", $$1, $$2}'

bootstrap: ## First-time setup: host deps check, git hooks, dev image
	@bash scripts/doctor.sh || { echo "fix the MISSING items above, then re-run make bootstrap"; exit 1; }
	@$(MAKE) --no-print-directory setup-hooks
	@$(MAKE) --no-print-directory image

doctor: ## Report what setup is missing, change nothing
	@bash scripts/doctor.sh

image: ## Build the dev image (content-addressed tag, idempotent)
	@docker image inspect $(IMAGE) >/dev/null 2>&1 && echo "$(IMAGE) is up to date" \
		|| docker build --target dev $(BUILD_ARGS) -t $(IMAGE) .

shell: guard-main-clone cache-dir ## Interactive shell inside the image
	@$(DOCKER_RUN) bash

exec: guard-main-clone cache-dir ## One-off command inside the image: make exec CMD='...'
	@test -n "$(CMD)" || { echo "usage: make exec CMD='...'"; exit 2; }
	@$(call in_image,bash -c '$(CMD)')

test: guard-main-clone cache-dir ## Run scripts/ci-test.sh, the exact suite CI runs (scope: LINT=1 SCRIPTS=1 COMMITS=1 ...)
	@$(call in_image,bash scripts/ci-test.sh)

ci-local: guard-main-clone cache-dir image ## The exact CI run, unscoped, in the ci image
	@$(DOCKER_BASE) $(IMAGE) bash scripts/ci-test.sh

compile: ## Build an example firmware and print flash, RAM and stack
	@$(call pending,compile,G10 and V4 in docs/00-design.md)

fuzz: guard-main-clone cache-dir ## Fuzz each target for 60 s (TARGET=name and R2S_FUZZ_SECONDS=n narrow it)
	@$(call in_image,env TARGET='$(TARGET)' R2S_FUZZ_SECONDS='$(R2S_FUZZ_SECONDS)' bash scripts/fuzz.sh)

size-report: ## Regenerate the flash, RAM and stack report
	@$(call pending,size-report,P6 in docs/00-design.md)

vectors: guard-main-clone cache-dir ## Validate every vector file and regenerate the fuzz seeds
	@$(call in_image,python3 tools/vectors_check.py)
	@$(call in_image,python3 tools/fuzz_corpus.py)

regen: cache-dir ## Regenerate every version-stamped generated file (goldens, example and cpp libraries, posix sources)
	@$(call in_image,python3 tools/refresh_generated.py)

docs-gen: ## Regenerate reference docs (CLI, vectors, bridge parameters)
	@$(call pending,docs-gen,G1 and H1 in docs/00-design.md)

fmt: guard-main-clone cache-dir ## Format the tree (ruff, clang-format)
	@$(call in_image,bash -c 'ruff format . && bash scripts/c-format.sh --fix')

lint: guard-main-clone cache-dir ## Linters: the LINT section of the suite
	@$(call in_image,env LINT=1 bash scripts/ci-test.sh)

check-warnings: guard-main-clone cache-dir ## Fail on any compiler warning (hard gate): core builds with -Werror, cppcheck
	@$(call in_image,bash scripts/check-warnings.sh)

setup-hooks: ## Install git hooks from .githooks/
	@git config core.hooksPath .githooks && echo "git hooks installed (core.hooksPath = .githooks)"

pre-pr-check: guard-main-clone cache-dir ## Takeoff ritual before opening a PR (BASE=<ref> PR_BODY=<file> PR_TITLE="<title>" TEST=1)
	@$(call in_image,bash scripts/pre-pr-check.sh)

guard-main-clone:
	@bash scripts/check-main-clone-build.sh

cache-dir:
	@mkdir -p $(CACHE) $(BUILD_DIR)
