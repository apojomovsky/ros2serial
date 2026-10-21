# Dev image: every toolchain the suite needs. Stages: base (system packages),
# dev (pinned Python tools, arduino-cli), ci (an empty alias of dev, so running
# locally is running CI), release (artifact builds; trimmed when L1 lands).
# Versions come from ci/versions.env as build arguments (see the Makefile).

ARG ROS_IMAGE=ros:jazzy-ros-base-noble
FROM ${ROS_IMAGE} AS base
ARG DEBIAN_FRONTEND=noninteractive
ARG AVR_GCC_VERSION
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-avr avr-libc binutils-avr simavr libsimavr-dev libelf-dev \
        build-essential clang clang-format clang-tidy cppcheck cmake ninja-build ccache \
        libclang-rt-18-dev $([ "$(dpkg --print-architecture)" = amd64 ] && echo gcc-multilib g++-multilib) \
        python3-venv python3-pip git curl ca-certificates make \
    && rm -rf /var/lib/apt/lists/* \
    && test "$(avr-gcc -dumpversion)" = "${AVR_GCC_VERSION}" \
    || { echo "avr-gcc is $(avr-gcc -dumpversion), ci/versions.env pins ${AVR_GCC_VERSION}" >&2; exit 1; }

FROM base AS dev
ARG RUFF_VERSION
ARG PYTEST_VERSION
ARG ACTIONLINT_PY_VERSION
ARG GIT_CLIFF_VERSION
ARG PYYAML_VERSION
ARG JSONSCHEMA_VERSION
ARG PYSERIAL_VERSION
ARG GCOVR_VERSION
ARG ARDUINO_CLI_VERSION
ARG ARDUINO_AVR_CORE_VERSION
# System site packages stay visible so the ROS Python packages (rclpy) work too.
RUN python3 -m venv --system-site-packages /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir \
        "ruff==${RUFF_VERSION}" "pytest==${PYTEST_VERSION}" "actionlint-py==${ACTIONLINT_PY_VERSION}" \
        "git-cliff==${GIT_CLIFF_VERSION}" "pyyaml==${PYYAML_VERSION}" "jsonschema==${JSONSCHEMA_VERSION}" \
        "pyserial==${PYSERIAL_VERSION}" "gcovr==${GCOVR_VERSION}"
ENV ARDUINO_DIRECTORIES_DATA=/opt/arduino \
    ARDUINO_DIRECTORIES_USER=/opt/arduino/user \
    ARDUINO_BUILD_CACHE_PATH=/tmp/arduino-build-cache
RUN curl -fsSL "https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz" \
        | tar -xz -C /usr/local/bin arduino-cli \
    && arduino-cli core update-index \
    && arduino-cli core install "arduino:avr@${ARDUINO_AVR_CORE_VERSION}" \
    && chmod -R a+rX /opt/arduino
# The container runs as the caller's uid, which has no home directory here.
RUN mkdir -p /tmp/home && chmod 1777 /tmp/home
ENV PATH=/opt/venv/bin:${PATH} \
    HOME=/tmp/home \
    R2S_IN_IMAGE=1

FROM dev AS ci

FROM dev AS release
