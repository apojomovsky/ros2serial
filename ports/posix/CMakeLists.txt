# The posix "virtual MCU": the unmodified core plus the posix port, as one
# host process the end-to-end tests launch on a pty (docs/00-design.md, C10).
cmake_minimum_required(VERSION 3.22)
project(r2s_posix C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(R2S_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
# test_reset.c is the host test builds' reset; the firmware never uses it.
file(GLOB R2S_CORE_SOURCES CONFIGURE_DEPENDS "${R2S_REPO_ROOT}/core/src/*.c")
list(REMOVE_ITEM R2S_CORE_SOURCES "${R2S_REPO_ROOT}/core/src/test_reset.c")

add_executable(r2s_posix_firmware
    src/r2s_posix_port.c
    src/r2s_posix_firmware.c
    "${R2S_REPO_ROOT}/core/tests/support/handwritten.c"
    "${R2S_REPO_ROOT}/core/tests/support/handwritten_firmware.c")
target_include_directories(r2s_posix_firmware PRIVATE
    "${R2S_REPO_ROOT}/core/include" "${R2S_REPO_ROOT}/core/tests/support" src)
# The firmware builds the same configuration the core suite tests: the codec
# with two nesting levels and a 512-byte maximum frame.
target_compile_definitions(r2s_posix_firmware PRIVATE R2S_WITH_CODEC R2S_MAX_NEST=2 R2S_MAX_FRAME=512)
target_compile_options(r2s_posix_firmware PRIVATE
    -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes -Wundef)
