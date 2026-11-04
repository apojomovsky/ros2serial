# Big-endian test image: the s390x cross toolchain plus qemu-user, so the
# nightly job builds the core for a big-endian target and runs its unit and
# vector tests under emulation. This image is separate from the everyday dev
# image on purpose: big-endian runs nightly only, never in a pull request
# check or the pre-PR ritual. Reproduce locally with: task bigendian.
# The versions stay pinned here as build arguments, not in ci/versions.env,
# which only a human may touch; this nightly-only image never gates a pull
# request or measures a budget.
ARG UBUNTU_IMAGE=ubuntu:24.04
FROM ${UBUNTU_IMAGE}
ARG DEBIAN_FRONTEND=noninteractive
# The cross toolchain and the emulator, pinned and checked after install:
# apt cannot pin these, so drift fails the build loudly instead of changing
# the nightly silently, as the dev image does for avr-gcc. QEMU_VERSION is
# the upstream version qemu-s390x --version prints, and GCC_S390X_VERSION is
# what s390x-linux-gnu-gcc -dumpfullversion prints. Both came from
# https://packages.ubuntu.com/noble-updates (qemu-user and
# gcc-13-s390x-linux-gnu) on 2026-11-04.
ARG QEMU_VERSION=8.2.2
ARG GCC_S390X_VERSION=13.3.0
# These mirror ci/versions.env, so the reference codec tests meet the same
# Python dependencies the pull request suite uses. scripts/run-bigendian.sh
# forwards the file's values as build arguments, so a pin bump there rebuilds
# this image with the new values; the defaults below match for direct docker
# builds. Only a human may touch versions.env itself.
ARG PYTEST_VERSION=9.1.1
ARG HYPOTHESIS_VERSION=6.170.12
ARG PYYAML_VERSION=6.0.3
ARG JSONSCHEMA_VERSION=4.26.0
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-s390x-linux-gnu qemu-user \
        build-essential clang-format cmake ninja-build \
        python3 python3-venv python3-pip git make \
    && rm -rf /var/lib/apt/lists/* \
    && test "$(qemu-s390x --version | sed -n '1s/.*version \([0-9.]*\).*/\1/p')" = "${QEMU_VERSION}" \
    || { echo "qemu is $(qemu-s390x --version | head -1), Dockerfile pins ${QEMU_VERSION}" >&2; exit 1; } \
    && test "$(s390x-linux-gnu-gcc -dumpfullversion)" = "${GCC_S390X_VERSION}" \
    || { echo "s390x gcc is $(s390x-linux-gnu-gcc -dumpfullversion), Dockerfile pins ${GCC_S390X_VERSION}" >&2; exit 1; }
# clang-format is unpinned, as in the dev image: the emit tests format only
# when it is installed, and skip otherwise.
RUN python3 -m venv /opt/bigendian \
    && /opt/bigendian/bin/pip install --no-cache-dir \
        "pytest==${PYTEST_VERSION}" "hypothesis==${HYPOTHESIS_VERSION}" \
        "pyyaml==${PYYAML_VERSION}" "jsonschema==${JSONSCHEMA_VERSION}" \
    && /opt/bigendian/bin/pytest --version
ENV PATH="/opt/bigendian/bin:${PATH}"
WORKDIR /src
