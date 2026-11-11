# Mutation testing image: Mull for C and C++, mutmut for Python. This image
# is separate from the everyday dev image on purpose: mutation testing runs
# nightly only, never in a pull request check or the pre-PR ritual, so its
# tools must not slow down or reshape what every contributor builds with.
# Reproduce locally with: docker build -f docker/mutation/Dockerfile -t r2s-mutation .
# The versions stay pinned here as build arguments. They live in this file
# and not in ci/versions.env because only a human may touch that file, while
# this nightly-only image never gates a pull request or measures a budget.
ARG UBUNTU_IMAGE=ubuntu:24.04
FROM ${UBUNTU_IMAGE}
ARG DEBIAN_FRONTEND=noninteractive
ARG CLANG_VERSION=18
ARG MULL_VERSION=0.37.0
ARG LLVM_VERSION=18.1.3
ARG MUTMUT_VERSION=3.8.1
# These mirror ci/versions.env, so the nightly mutants meet the same test
# dependencies the pull request suite uses. A pin bump there updates these
# here in the same migration; only a human may touch versions.env itself.
ARG PYTEST_VERSION=9.1.1
ARG HYPOTHESIS_VERSION=6.170.12
ARG PYYAML_VERSION=6.0.3
ARG JSONSCHEMA_VERSION=4.26.0
COPY scripts/apt-install.sh /usr/local/bin/apt-install.sh
RUN apt-install.sh update && apt-install.sh install -y --no-install-recommends \
        clang-${CLANG_VERSION} cmake ninja-build python3 python3-venv python3-pip \
        gcc g++ curl ca-certificates make git \
        libgtest-dev libyaml-cpp-dev \
    && rm -rf /var/lib/apt/lists/*
# libgtest-dev ships only sources: the bridge harness links the static
# libraries, so build them once here instead of on every nightly run.
RUN cmake -S /usr/src/googletest/googletest -B /tmp/gtest-build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/gtest-build -j"$(nproc)" \
    && cp /tmp/gtest-build/lib/libgtest*.a /usr/lib/x86_64-linux-gnu/ \
    && rm -rf /tmp/gtest-build
# Both debs: the LLVM build depends on the base package and the LLVM
# libraries, so apt resolves them instead of a bare dpkg install.
RUN curl -sSL -o /tmp/mull.deb \
        https://github.com/mull-project/mull/releases/download/${MULL_VERSION}/Mull-${MULL_VERSION}-ubuntu-amd64-24.04.deb \
    && curl -sSL -o /tmp/mull-llvm.deb \
        https://github.com/mull-project/mull/releases/download/${MULL_VERSION}/Mull-18-${MULL_VERSION}-LLVM-${LLVM_VERSION}-ubuntu-amd64-24.04.deb \
    && apt-install.sh update && apt-install.sh install -y /tmp/mull.deb /tmp/mull-llvm.deb \
    && rm /tmp/mull.deb /tmp/mull-llvm.deb \
    && mull-runner --version
RUN python3 -m venv /opt/mutmut \
    && /opt/mutmut/bin/pip install --no-cache-dir "mutmut==${MUTMUT_VERSION}" \
        "pytest==${PYTEST_VERSION}" "hypothesis==${HYPOTHESIS_VERSION}" \
        "pyyaml==${PYYAML_VERSION}" "jsonschema==${JSONSCHEMA_VERSION}" \
    && /opt/mutmut/bin/mutmut --version
ENV PATH="/opt/mutmut/bin:${PATH}"
WORKDIR /src
