# The C99 core and its host test builds. scripts/core-test.sh configures one
# build per CI configuration through the options below and CMAKE_C_FLAGS
# (-m32 for the 32-bit build).
cmake_minimum_required(VERSION 3.22)
# C++ only to check that the headers can be used from C++ (the bridge, cpp/).
project(r2s_core LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(R2S_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(R2S_COVERAGE "Build with line and branch coverage instrumentation" OFF)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# This project is the host test build: the core itself compiles with the test
# hooks (docs/01-testing.md, section 2.1). Firmware builds never use it.
add_compile_definitions(R2S_TESTING)
# A frame limit above 254 makes the decoded-too-large drop reachable, because
# below it the buffer bound refuses such a frame first (FRM-12). Firmware takes
# the value from the generated configuration.
add_compile_definitions(R2S_MAX_FRAME=512)
# The test firmware configures topics with dispatch, and nests Twist two deep;
# the generated configuration sets both per firmware.
add_compile_definitions(R2S_WITH_CODEC R2S_MAX_NEST=2 R2S_WITH_LOG)
set(R2S_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(R2S_WARNINGS
    -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes -Wundef)

if(R2S_SANITIZE)
  # Any finding aborts the test, so a sanitizer report is a failing test.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(R2S_COVERAGE)
  add_compile_options(--coverage -O0)
  add_link_options(--coverage)
endif()

file(GLOB_RECURSE R2S_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_library(r2s_core STATIC ${R2S_CORE_SOURCES})
target_include_directories(r2s_core PUBLIC include)
target_compile_options(r2s_core PRIVATE ${R2S_WARNINGS})

enable_testing()
add_subdirectory(tests)
