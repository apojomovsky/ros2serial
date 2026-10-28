# The posix "virtual MCU": the unmodified core plus the posix port, as one
# host process the end-to-end tests launch on a pty (docs/00-design.md, C10).
cmake_minimum_required(VERSION 3.22)
project(r2s_posix C CXX)

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
file(STRINGS src/r2s_generated.h R2S_TOPIC_MAX_COUNT_LINE REGEX "^#define R2S_TOPIC_MAX_COUNT [0-9]+u$")
# A generated header changes without any CMakeLists edit, so the limits above
# go stale unless the configure step watches it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/r2s_generated.h)
if(NOT R2S_MAX_FRAME_LINE OR NOT R2S_MAX_NEST_LINE)
  message(FATAL_ERROR "r2s_generated.h names no frame or nesting limit")
endif()
string(REGEX MATCH "^#define R2S_MAX_FRAME ([0-9]+)u$" _ignored "${R2S_MAX_FRAME_LINE}")
set(R2S_MAX_FRAME "${CMAKE_MATCH_1}")
string(REGEX MATCH "^#define R2S_MAX_NEST ([0-9]+)u$" _ignored "${R2S_MAX_NEST_LINE}")
set(R2S_MAX_NEST "${CMAKE_MATCH_1}")
set(R2S_GENERATED_DEFS R2S_MAX_FRAME=${R2S_MAX_FRAME} R2S_MAX_NEST=${R2S_MAX_NEST})
if(R2S_TOPIC_MAX_COUNT_LINE)
  string(REGEX MATCH "^#define R2S_TOPIC_MAX_COUNT ([0-9]+)u$" _ignored
    "${R2S_TOPIC_MAX_COUNT_LINE}")
  list(APPEND R2S_GENERATED_DEFS R2S_TOPIC_MAX_COUNT=${CMAKE_MATCH_1})
endif()
target_compile_definitions(r2s_posix_firmware PRIVATE R2S_WITH_CODEC ${R2S_GENERATED_DEFS})
target_compile_options(r2s_posix_firmware PRIVATE
    -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes -Wundef)

# The runtime registration path announces the same topics through the C++
# layer: the firmware registers its publishers and subscriptions as objects,
# and the end-to-end suite runs the same scenarios against it. The message
# headers name the bundled tables, so the build emits the bundle for the
# pinned distro first; the type hashes the headers carry match the
# checked-in generated firmware's, which is what keeps the bridge's view of
# the types identical on both paths.
find_program(R2S_PYTHON python3 REQUIRED)
set(R2S_BUNDLE_DIR "${CMAKE_BINARY_DIR}/r2s_bundle")
set(ENV{PYTHONPATH} "${R2S_REPO_ROOT}/gen")
execute_process(
    COMMAND ${R2S_PYTHON} -m r2s_gen.cli bundle --distro jazzy --out "${R2S_BUNDLE_DIR}"
    RESULT_VARIABLE R2S_BUNDLE_RESULT
    OUTPUT_VARIABLE R2S_BUNDLE_OUTPUT
    ERROR_VARIABLE R2S_BUNDLE_ERROR)
if(NOT R2S_BUNDLE_RESULT EQUAL 0)
  message(FATAL_ERROR "r2s-gen bundle failed: ${R2S_BUNDLE_ERROR}")
endif()
file(STRINGS "${R2S_BUNDLE_DIR}/src/r2s_bundled.h" R2S_BUNDLE_MAX_FRAME_LINE
    REGEX "^#define R2S_MAX_FRAME [0-9]+u$")
file(STRINGS "${R2S_BUNDLE_DIR}/src/r2s_bundled.h" R2S_BUNDLE_MAX_NEST_LINE
    REGEX "^#define R2S_MAX_NEST [0-9]+u$")
if(NOT R2S_BUNDLE_MAX_FRAME_LINE OR NOT R2S_BUNDLE_MAX_NEST_LINE)
  message(FATAL_ERROR "the emitted bundle names no frame or nesting limit")
endif()
string(REGEX MATCH "^#define R2S_MAX_FRAME ([0-9]+)u$" _ignored "${R2S_BUNDLE_MAX_FRAME_LINE}")
set(R2S_BUNDLE_MAX_FRAME "${CMAKE_MATCH_1}")
string(REGEX MATCH "^#define R2S_MAX_NEST ([0-9]+)u$" _ignored "${R2S_BUNDLE_MAX_NEST_LINE}")
set(R2S_BUNDLE_MAX_NEST "${CMAKE_MATCH_1}")

add_executable(r2s_posix_runtime_firmware
    ${R2S_CORE_SOURCES}
    "${R2S_REPO_ROOT}/cpp/src/runtime.cpp"
    "${R2S_BUNDLE_DIR}/src/r2s_bundled.c"
    src/r2s_posix_port.c
    src/r2s_posix_runtime_firmware.cpp)
target_include_directories(r2s_posix_runtime_firmware PRIVATE
    "${R2S_REPO_ROOT}/core/include" "${R2S_REPO_ROOT}/cpp/include"
    "${R2S_BUNDLE_DIR}/src" src)
target_compile_definitions(r2s_posix_runtime_firmware PRIVATE
    R2S_WITH_CODEC R2S_MAX_FRAME=${R2S_BUNDLE_MAX_FRAME} R2S_MAX_NEST=${R2S_BUNDLE_MAX_NEST})
target_compile_features(r2s_posix_runtime_firmware PRIVATE cxx_std_11)
set_target_properties(r2s_posix_runtime_firmware PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(r2s_posix_runtime_firmware PRIVATE
    -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow -Wundef)

enable_testing()
# The tick lives in the port, not the core, so its checks link only the port
# and run with no pty and no boot file.
add_executable(r2s_posix_port_test tests/test_posix_port_tick.c src/r2s_posix_port.c)
target_include_directories(r2s_posix_port_test PRIVATE "${R2S_REPO_ROOT}/core/include" src)
target_compile_options(r2s_posix_port_test PRIVATE
    -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes -Wundef)
# The pin on the tick's clock source lives in the test's wrap of
# clock_gettime, so only this target carries it; the firmware is untouched.
target_link_options(r2s_posix_port_test PRIVATE "LINKER:--wrap=clock_gettime")
add_test(NAME posix_port_tick COMMAND r2s_posix_port_test)
