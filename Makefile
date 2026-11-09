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
# Scope and suite variables forwarded into the dev image, one per line in
# scripts/pass-vars.list so concurrent branches appending different variables
# keep both on merge.
PASS_VARS := $(shell grep -v '^#' scripts/pass-vars.list | tr '\n' ' ')

DOCKER_BASE = docker run --rm $(TTY) --user $$(id -u):$$(id -g) \
	-v $(REPO_ROOT):$(REPO_ROOT) -w $(CURDIR) -v $(CACHE):$(CACHE) -e R2S_BUILD_DIR=$(BUILD_DIR) -e R2S_CACHE_DIR=$(CACHE) -e R2S_HOST_MOUNT=$(REPO_ROOT)
DOCKER_RUN = $(DOCKER_BASE) $(foreach v,$(PASS_VARS),-e $(v)) $(IMAGE)
in_image = $(if $(R2S_IN_IMAGE),$(1),$(DOCKER_RUN) $(1))
pending = echo "make $(1): not available until $(2)"; exit 1

# A misspelled scope must fail with the valid scopes, not silently run nothing
# or everything. Only command-line variables are checked, so environment
# knobs pass through untouched.
_CMDLINE_VARS := $(foreach v,$(.VARIABLES),$(if $(filter command line,$(origin $v)),$v))
_SCOPE_ERROR := $(shell bash scripts/check-scope-vars.sh $(_CMDLINE_VARS))
$(if $(_SCOPE_ERROR),$(error $(_SCOPE_ERROR)))

.PHONY: help bootstrap doctor check test test-core test-gen test-bridge test-cpp test-scripts \
	test-all e2e-posix e2e-avr pytest ctest fmt shell image exec ci-local compile fuzz size-report \
	vectors regen docs-gen lint check-warnings setup-hooks pre-pr-check guard-main-clone cache-dir

##@Everyday

help: ## Show this help: everyday targets first, then CI and agent targets
	@awk 'BEGIN {FS = ":.*## "} /^##@/ {printf "\n%s\n", substr($$0, 4)} /^[a-z][a-z0-9-]*:.*## / {printf "  %-16s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

bootstrap: ## First-time setup: host deps check, git hooks, dev image
	@bash scripts/doctor.sh || { echo "fix the MISSING items above, then re-run make bootstrap"; exit 1; }
	@$(MAKE) --no-print-directory setup-hooks
	@$(MAKE) --no-print-directory image

doctor: ## Report what setup is missing, change nothing
	@bash scripts/doctor.sh

check: guard-main-clone cache-dir ## Run the stale tasks for the current tree
	@$(call in_image,bash scripts/check.sh)

test: guard-main-clone cache-dir ## Run the stale suite tasks (scope: LINT=1 SCRIPTS=1 COMMITS=1 ...; R2S_FULL=1 forces all)
	@$(call in_image,bash scripts/run-tasks.sh)

test-core: guard-main-clone cache-dir ## Run the CORE scope: same as make test CORE=1
	@$(MAKE) --no-print-directory test CORE=1

test-gen: guard-main-clone cache-dir ## Run the GEN scope: same as make test GEN=1
	@$(MAKE) --no-print-directory test GEN=1

test-bridge: guard-main-clone cache-dir ## Run the BRIDGE scope: same as make test BRIDGE=1
	@$(MAKE) --no-print-directory test BRIDGE=1

test-cpp: guard-main-clone cache-dir ## Run the CPP scope: same as make test CPP=1
	@$(MAKE) --no-print-directory test CPP=1

test-scripts: guard-main-clone cache-dir ## Run the SCRIPTS scope: same as make test SCRIPTS=1
	@$(MAKE) --no-print-directory test SCRIPTS=1

test-all: guard-main-clone cache-dir ## Run the whole suite, unscoped
	@$(MAKE) --no-print-directory test

e2e-posix: guard-main-clone cache-dir ## Run the posix end-to-end suite: same as make test E2E=posix
	@$(MAKE) --no-print-directory test E2E=posix

e2e-avr: guard-main-clone cache-dir ## Run the avr end-to-end suite: same as make test E2E=avr
	@$(MAKE) --no-print-directory test E2E=avr

pytest: guard-main-clone cache-dir ## Run one pytest file or node: make pytest ARGS='<path>[::name] [-k expr]'
	@test -n "$(ARGS)" || { echo "usage: make pytest ARGS='<path>[::name] [-k expr]'"; exit 2; }
	@$(call in_image,bash scripts/pytest-one.sh '$(ARGS)')

ctest: guard-main-clone cache-dir ## Rerun core unit tests by name: make ctest ARGS='<regex>'
	@test -n "$(ARGS)" || { echo "usage: make ctest ARGS='<regex>'"; exit 2; }
	@$(call in_image,bash scripts/ctest-one.sh '$(ARGS)')

fmt: guard-main-clone cache-dir ## Format the tree (ruff, uncrustify)
	@$(call in_image,bash -c 'ruff format . && bash scripts/c-format.sh --fix')

shell: guard-main-clone cache-dir ## Interactive shell inside the image
	@$(DOCKER_RUN) bash

##@CI and agents

image: ## Build the dev image (content-addressed tag, idempotent)
	@docker image inspect $(IMAGE) >/dev/null 2>&1 && echo "$(IMAGE) is up to date" \
		|| docker build --target dev $(BUILD_ARGS) -t $(IMAGE) .

exec: guard-main-clone cache-dir ## One-off command inside the image: make exec CMD='...'
	@test -n "$(CMD)" || { echo "usage: make exec CMD='...'"; exit 2; }
	@$(call in_image,bash -c '$(CMD)')

ci-local: guard-main-clone cache-dir image ## The exact CI run, unscoped, in the ci image
	@$(DOCKER_BASE) -e R2S_FULL=1 $(IMAGE) bash scripts/run-tasks.sh

compile: ## Not built yet (pending G10 and V4): will build an example firmware
	@$(call pending,compile,G10 and V4 in docs/00-design.md)

fuzz: guard-main-clone cache-dir ## Fuzz each target for 60 s (TARGET=name and R2S_FUZZ_SECONDS=n narrow it)
	@$(call in_image,env TARGET='$(TARGET)' R2S_FUZZ_SECONDS='$(R2S_FUZZ_SECONDS)' bash scripts/fuzz.sh)

size-report: guard-main-clone cache-dir ## Regenerate the flash, RAM and stack report
	@$(call in_image,bash scripts/budgets-check.sh --report)

vectors: guard-main-clone cache-dir ## Validate every vector file and regenerate the fuzz seeds
	@$(call in_image,python3 tools/vectors_check.py)
	@$(call in_image,python3 tools/fuzz_corpus.py)

regen: cache-dir ## Regenerate every version-stamped generated file (goldens, example and cpp libraries, posix sources)
	@$(call in_image,python3 tools/refresh_generated.py)

docs-gen: ## Not built yet (pending G1 and H1): will regenerate reference docs
	@$(call pending,docs-gen,G1 and H1 in docs/00-design.md)

lint: guard-main-clone cache-dir ## Linters: the LINT section of the suite
	@$(MAKE) --no-print-directory test LINT=1

lint-docs: guard-main-clone cache-dir ## Check mermaid blocks in the docs with mermaid's own parser (needs host docker)
	@bash scripts/lint-docs.sh

bigendian: guard-main-clone cache-dir ## Cross-build the core for s390x and run the suite under qemu (host-only, needs docker)
	@bash scripts/run-bigendian.sh

check-warnings: guard-main-clone cache-dir ## Fail on any compiler warning (hard gate): core builds with -Werror, cppcheck
	@$(call in_image,bash scripts/check-warnings.sh)

setup-hooks: ## Install git hooks and this clone's merge drivers
	@bash scripts/setup-hooks.sh

pre-pr-check: guard-main-clone cache-dir ## Takeoff ritual before opening a PR (BASE=<ref> PR_BODY=<file> PR_TITLE="<title>" TEST=1)
	@$(call in_image,bash scripts/pre-pr-check.sh)

guard-main-clone:
	@bash scripts/check-main-clone-build.sh

cache-dir:
	@mkdir -p $(CACHE) $(BUILD_DIR)
