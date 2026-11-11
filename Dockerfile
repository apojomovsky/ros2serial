# Dev image: every toolchain the suite needs. Stages: base (system packages),
# dev (pinned Python tools, arduino-cli), ci (an empty alias of dev, so running
# locally is running CI), release (artifact builds; trimmed when L1 lands).
# Versions come from ci/versions.env as build arguments (see the Makefile).

ARG ROS_IMAGE=ros:jazzy-ros-base-noble
FROM ${ROS_IMAGE} AS base
ARG DEBIAN_FRONTEND=noninteractive
ARG AVR_GCC_VERSION
COPY scripts/apt-install.sh /usr/local/bin/apt-install.sh
RUN apt-install.sh update && apt-install.sh install -y --no-install-recommends \
        gcc-avr avr-libc binutils-avr simavr libsimavr-dev libelf-dev \
        build-essential clang clang-format clang-tidy cppcheck cmake ninja-build ccache \
        libclang-rt-18-dev $([ "$(dpkg --print-architecture)" = amd64 ] && echo gcc-multilib g++-multilib) \
        python3-venv python3-pip git curl ca-certificates make \
        ros-$ROS_DISTRO-rmw-cyclonedds-cpp ros-$ROS_DISTRO-rmw-zenoh-cpp \
    && rm -rf /var/lib/apt/lists/* \
    && test "$(avr-gcc -dumpversion)" = "${AVR_GCC_VERSION}" \
    || { echo "avr-gcc is $(avr-gcc -dumpversion), ci/versions.env pins ${AVR_GCC_VERSION}" >&2; exit 1; }
ARG RISCV_XPACK_VERSION
ARG RISCV_XPACK_SHA256
ARG RISCV_GCC_VERSION
RUN curl -fsSL -o /tmp/riscv-xpack.tar.gz "https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v${RISCV_XPACK_VERSION}/xpack-riscv-none-elf-gcc-${RISCV_XPACK_VERSION}-linux-x64.tar.gz" \
    && echo "${RISCV_XPACK_SHA256}  /tmp/riscv-xpack.tar.gz" | sha256sum -c - \
    && mkdir -p /opt/riscv-xpack && tar -xz -C /opt/riscv-xpack --strip-components=1 -f /tmp/riscv-xpack.tar.gz \
    && rm /tmp/riscv-xpack.tar.gz \
    && test "$(/opt/riscv-xpack/bin/riscv-none-elf-gcc -dumpversion)" = "${RISCV_GCC_VERSION}" \
    || { echo "riscv-none-elf-gcc is $(/opt/riscv-xpack/bin/riscv-none-elf-gcc -dumpversion), ci/versions.env pins ${RISCV_GCC_VERSION}" >&2; exit 1; }
ENV PATH=/opt/riscv-xpack/bin:${PATH}

FROM base AS dev
ARG RUFF_VERSION
ARG PYTEST_VERSION
ARG ACTIONLINT_PY_VERSION
ARG GIT_CLIFF_VERSION
ARG PYYAML_VERSION
ARG HYPOTHESIS_VERSION
ARG JSONSCHEMA_VERSION
ARG PYSERIAL_VERSION
ARG GCOVR_VERSION
ARG COVERAGE_VERSION
ARG ARDUINO_CLI_VERSION
ARG ARDUINO_AVR_CORE_VERSION
ARG TASK_VERSION
ARG TASK_SHA256
ARG ARM_GCC_VERSION
ARG ARM_GCC_SHA256
# System site packages stay visible so the ROS Python packages (rclpy) work too.
RUN python3 -m venv --system-site-packages /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir \
        "ruff==${RUFF_VERSION}" "pytest==${PYTEST_VERSION}" "actionlint-py==${ACTIONLINT_PY_VERSION}" \
        "git-cliff==${GIT_CLIFF_VERSION}" "pyyaml==${PYYAML_VERSION}" "jsonschema==${JSONSCHEMA_VERSION}" \
        "pyserial==${PYSERIAL_VERSION}" "gcovr==${GCOVR_VERSION}" "hypothesis==${HYPOTHESIS_VERSION}" \
        "coverage==${COVERAGE_VERSION}"
ENV ARDUINO_DIRECTORIES_DATA=/opt/arduino \
    ARDUINO_DIRECTORIES_USER=/opt/arduino/user \
    ARDUINO_BUILD_CACHE_PATH=/tmp/arduino-build-cache
RUN curl -fsSL "https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz" \
        | tar -xz -C /usr/local/bin arduino-cli \
    && arduino-cli core update-index \
    && arduino-cli core install "arduino:avr@${ARDUINO_AVR_CORE_VERSION}" \
    && chmod -R a+rX /opt/arduino
# The suite runner. The tarball is checked against its pinned hash before
# anything is extracted, never piped from curl into a shell.
RUN curl -fsSL -o /tmp/task.tar.gz "https://github.com/go-task/task/releases/download/v${TASK_VERSION}/task_linux_amd64.tar.gz" \
    && echo "${TASK_SHA256}  /tmp/task.tar.gz" | sha256sum -c - \
    && tar -xz -C /usr/local/bin -f /tmp/task.tar.gz task \
    && rm /tmp/task.tar.gz \
    && task --version
# The Arm toolchain the STM32 and RP2040 firmware builds with. The tarball
# is checked against its pinned hash before anything is extracted, and the
# version directory it carries keeps one pin beside the next on upgrades.
RUN curl -fsSL -o /tmp/arm-gcc.tar.gz "https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v${ARM_GCC_VERSION}/xpack-arm-none-eabi-gcc-${ARM_GCC_VERSION}-linux-x64.tar.gz" \
    && echo "${ARM_GCC_SHA256}  /tmp/arm-gcc.tar.gz" | sha256sum -c - \
    && tar -xz -C /opt -f /tmp/arm-gcc.tar.gz \
    && rm /tmp/arm-gcc.tar.gz \
    && /opt/xpack-arm-none-eabi-gcc-${ARM_GCC_VERSION}/bin/arm-none-eabi-gcc --version
ENV PATH=/opt/xpack-arm-none-eabi-gcc-${ARM_GCC_VERSION}/bin:${PATH}
# The container runs as the caller's uid, which has no home directory here.
RUN mkdir -p /tmp/home && chmod 1777 /tmp/home
ENV PATH=/opt/venv/bin:${PATH} \
    HOME=/tmp/home \
    R2S_IN_IMAGE=1

FROM dev AS ci

FROM dev AS release
