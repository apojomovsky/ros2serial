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
    ${R2S_CORE_SOURCES}
    src/r2s_posix_port.c
    src/r2s_posix_firmware.c
    src/r2s_generated.c)
target_include_directories(r2s_posix_firmware PRIVATE
    "${R2S_REPO_ROOT}/core/include" src)
# The frame and nesting limits live in the generated header, but the core
# translation units never include it, so the build reads them out as defines:
# macros are per translation unit, and a default here would silently disagree.
file(STRINGS src/r2s_generated.h R2S_MAX_FRAME_LINE REGEX "^#define R2S_MAX_FRAME [0-9]+u$")
file(STRINGS src/r2s_generated.h R2S_MAX_NEST_LINE REGEX "^#define R2S_MAX_NEST [0-9]+u$")
file(STRINGS src/r2s_generated.h R2S_SESSION_MAX_TOPICS_LINE REGEX "^#define R2S_SESSION_MAX_TOPICS [0-9]+u$")
if(NOT R2S_MAX_FRAME_LINE OR NOT R2S_MAX_NEST_LINE)
  message(FATAL_ERROR "r2s_generated.h names no frame or nesting limit")
endif()
string(REGEX MATCH "^#define R2S_MAX_FRAME ([0-9]+)u$" _ignored "${R2S_MAX_FRAME_LINE}")
set(R2S_MAX_FRAME "${CMAKE_MATCH_1}")
string(REGEX MATCH "^#define R2S_MAX_NEST ([0-9]+)u$" _ignored "${R2S_MAX_NEST_LINE}")
set(R2S_MAX_NEST "${CMAKE_MATCH_1}")
set(R2S_GENERATED_DEFS R2S_MAX_FRAME=${R2S_MAX_FRAME} R2S_MAX_NEST=${R2S_MAX_NEST})
if(R2S_SESSION_MAX_TOPICS_LINE)
  string(REGEX MATCH "^#define R2S_SESSION_MAX_TOPICS ([0-9]+)u$" _ignored
    "${R2S_SESSION_MAX_TOPICS_LINE}")
  list(APPEND R2S_GENERATED_DEFS R2S_SESSION_MAX_TOPICS=${CMAKE_MATCH_1})
endif()
target_compile_definitions(r2s_posix_firmware PRIVATE R2S_WITH_CODEC ${R2S_GENERATED_DEFS})
target_compile_options(r2s_posix_firmware PRIVATE
    -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes -Wundef)

enable_testing()
# The tick lives in the port, not the core, so its checks link only the port
# and run with no pty and no boot file.
add_executable(r2s_posix_port_test tests/test_posix_port_tick.c src/r2s_posix_port.c)
target_include_directories(r2s_posix_port_test PRIVATE "${R2S_REPO_ROOT}/core/include" src)
target_compile_options(r2s_posix_port_test PRIVATE
    -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes -Wundef)
add_test(NAME posix_port_tick COMMAND r2s_posix_port_test)
