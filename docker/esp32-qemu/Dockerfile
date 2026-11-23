# ESP32 QEMU check image: the dev image plus Espressif's QEMU fork, so the
# spike runs arduino_button_led for the ESP32 Dev Module under emulation
# against the bridge. The dev image itself is unchanged: this image layers
# the emulator binary and the two shared libraries it needs on top of it.
# The release stays pinned in ci/versions.env, which the check forwards as
# build arguments; the tarball hash is checked before anything is extracted
# from it. Reproduce locally with: bash scripts/esp32-qemu-check.sh.
# A nightly job is a follow-up; no pull request job builds this image.
ARG DEV_IMAGE
FROM ${DEV_IMAGE}
ARG ESP_QEMU_VERSION
ARG ESP_QEMU_SHA256
# The emulator links SDL2 and slirp dynamically, and the dev image carries
# neither: both join here as runtime libraries only. The asset name swaps
# the release tag's dashes for underscores, so the tag
# esp-develop-9.2.2-20261109 fetches
# qemu-xtensa-softmmu-esp_develop_9.2.2_20261109-x86_64-linux-gnu.tar.xz.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsdl2-2.0-0 libslirp0 \
    && rm -rf /var/lib/apt/lists/* \
    && asset="qemu-xtensa-softmmu-$(echo "${ESP_QEMU_VERSION}" | tr '-' '_')-x86_64-linux-gnu.tar.xz" \
    && curl -fsSL -o /tmp/qemu-xtensa.tar.xz \
        "https://github.com/espressif/qemu/releases/download/${ESP_QEMU_VERSION}/${asset}" \
    && echo "${ESP_QEMU_SHA256}  /tmp/qemu-xtensa.tar.xz" | sha256sum -c - \
    && mkdir -p /opt/qemu-esp32 \
    && tar -xJ -C /opt/qemu-esp32 --strip-components=1 -f /tmp/qemu-xtensa.tar.xz \
    && rm /tmp/qemu-xtensa.tar.xz \
    && qemu_version="$(echo "${ESP_QEMU_VERSION}" | sed -n 's/^esp-develop-\([0-9.]*\)-.*/\1/p')" \
    && /opt/qemu-esp32/bin/qemu-system-xtensa --version | grep -q "${qemu_version}" \
    || { echo "qemu-system-xtensa is not ${ESP_QEMU_VERSION}" >&2; exit 1; }
ENV PATH=/opt/qemu-esp32/bin:${PATH}
