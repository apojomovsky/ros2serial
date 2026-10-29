# The C++ layer's host test build: the runtime API over the core and the
# fake port. scripts/cpp-test.sh configures one build per CI configuration.
cmake_minimum_required(VERSION 3.22)
project(r2s_cpp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_compile_definitions(R2S_TESTING)
# The same feature set the core's host tests use, so the runtime speaks the
# same protocol there as on a board.
add_compile_definitions(R2S_WITH_CODEC R2S_MAX_NEST=2 R2S_WITH_LOG)
set(R2S_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(R2S_WARNINGS -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow -Wundef)

option(R2S_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(R2S_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# The core, the runtime layer and the fake port. The handwritten dispatch of
# the core's tests stays out: the runtime brings its own, and two of them
# would collide.
file(GLOB R2S_CORE_SOURCES CONFIGURE_DEPENDS "${R2S_REPO_ROOT}/core/src/*.c")
add_library(r2s_core STATIC ${R2S_CORE_SOURCES})
target_include_directories(r2s_core PUBLIC "${R2S_REPO_ROOT}/core/include")
target_compile_options(r2s_core PRIVATE ${R2S_WARNINGS})

add_library(r2s_runtime STATIC src/runtime.cpp)
target_include_directories(r2s_runtime PUBLIC include)
target_compile_options(r2s_runtime PRIVATE ${R2S_WARNINGS})
target_link_libraries(r2s_runtime PUBLIC r2s_core)

enable_testing()

# The test support: the fake port, the frame helpers and Unity. The
# dispatch stub of the core's tests is left out, since the runtime
# provides the dispatch the core calls.
add_library(unity STATIC "${R2S_REPO_ROOT}/core/tests/vendor/unity/unity.c")
target_include_directories(unity SYSTEM PUBLIC "${R2S_REPO_ROOT}/core/tests/vendor/unity")

add_library(r2s_cpp_test_support STATIC
            "${R2S_REPO_ROOT}/core/tests/support/bytes.c"
            "${R2S_REPO_ROOT}/core/tests/support/fake_port.c"
            "${R2S_REPO_ROOT}/core/tests/support/fake_port_unity.c"
            "${R2S_REPO_ROOT}/core/tests/support/frames.c"
            "${R2S_REPO_ROOT}/core/tests/support/handwritten.c"
            "${R2S_REPO_ROOT}/core/tests/support/rcobs.c")
target_include_directories(r2s_cpp_test_support PUBLIC "${R2S_REPO_ROOT}/core/tests/support")
target_compile_options(r2s_cpp_test_support PRIVATE ${R2S_WARNINGS})
target_link_libraries(r2s_cpp_test_support PUBLIC unity r2s_core)
# Unity calls setUp and tearDown, so they are linked as an object, not from
# an archive. This layer's hooks reset the runtime registry too, which the
# core's own hooks do not know about.
target_sources(r2s_cpp_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/hooks.cpp")

set(R2S_RUNNER_TOOL "${R2S_REPO_ROOT}/tools/unity_runner.py")

# One test executable per test source, with a runner the tool generates.
function(r2s_cpp_unit_test source)
  get_filename_component(name "${source}" NAME_WE)
  # The runner is C++, so it declares the test functions with the same
  # linkage the C++ test file defines them with.
  set(runner "${CMAKE_CURRENT_BINARY_DIR}/${name}_runner.cpp")
  add_custom_command(
    OUTPUT "${runner}"
    COMMAND Python3::Interpreter "${R2S_RUNNER_TOOL}" tests "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${runner}"
    DEPENDS "${source}" "${R2S_RUNNER_TOOL}"
    VERBATIM)
  add_executable(${name} "${source}" "${runner}")
  target_compile_options(${name} PRIVATE ${R2S_WARNINGS})
  target_link_libraries(${name} PRIVATE r2s_cpp_test_support r2s_runtime)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${R2S_RUNNER_TOOL}" names "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    OUTPUT_VARIABLE test_functions OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  string(REPLACE "\n" ";" test_functions "${test_functions}")
  foreach(fn ${test_functions})
    add_test(NAME ${name}.${fn} COMMAND ${name} ${fn})
  endforeach()
endfunction()

r2s_cpp_unit_test(tests/test_runtime.cpp)

# The bundled tables the static test announces: the pinned generator output,
# so the test speaks the tables the Arduino library ships. The frozen header
# in include/ is identical to that output, so the C file compiles against it.
add_library(r2s_bundled STATIC "${R2S_REPO_ROOT}/gen/tests/golden/bundle/r2s_bundled.c")
target_include_directories(r2s_bundled PUBLIC include)
target_compile_options(r2s_bundled PRIVATE ${R2S_WARNINGS})
target_link_libraries(r2s_bundled PUBLIC r2s_core)

r2s_cpp_unit_test(tests/test_static.cpp)
target_link_libraries(test_static PRIVATE r2s_bundled)
