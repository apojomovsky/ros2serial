# Unit tests (test_*.c, one executable each), must-fail tests (must_fail/*.c,
# which pass only by failing with the expected message) and vector tests (one
# executable per vector kind and check). Runners come from tools/unity_runner.py.

# Unity keeps its own style, so it builds without the core's warning flags.
add_library(unity STATIC vendor/unity/unity.c)
target_include_directories(unity SYSTEM PUBLIC vendor/unity)

# The ring is compiled only with R2S_WITH_RX_RING, so its test builds rx.c a
# second time with the switch on instead of changing the configuration every
# other test shares.
add_library(r2s_rx_ring STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../src/rx.c")
target_include_directories(r2s_rx_ring PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_definitions(r2s_rx_ring PUBLIC R2S_WITH_RX_RING)
target_compile_options(r2s_rx_ring PRIVATE ${R2S_WARNINGS})

add_library(r2s_test_support STATIC support/bytes.c support/fake_port.c support/fake_port_unity.c
                                       support/frames.c support/rcobs.c)
target_include_directories(r2s_test_support PUBLIC support)
target_compile_options(r2s_test_support PRIVATE ${R2S_WARNINGS})
target_link_libraries(r2s_test_support PUBLIC unity r2s_core)
# Unity calls setUp and tearDown, so they are linked as an object, not from an archive.
target_sources(r2s_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support/hooks.c")

# Test functions are found by the generated runner, not declared in headers.
set(R2S_TEST_WARNINGS ${R2S_WARNINGS})
list(REMOVE_ITEM R2S_TEST_WARNINGS -Wmissing-prototypes)

set(R2S_RUNNER_TOOL "${R2S_REPO_ROOT}/tools/unity_runner.py")

function(r2s_unit_test source)
  get_filename_component(name "${source}" NAME_WE)
  set(runner "${CMAKE_CURRENT_BINARY_DIR}/${name}_runner.c")
  add_custom_command(
    OUTPUT "${runner}"
    COMMAND Python3::Interpreter "${R2S_RUNNER_TOOL}" tests "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${runner}"
    DEPENDS "${source}" "${R2S_RUNNER_TOOL}"
    VERBATIM)
  add_executable(${name} "${source}" "${runner}")
  target_compile_options(${name} PRIVATE ${R2S_TEST_WARNINGS})
  # A test whose build is its own names what it links instead of the shared
  # fake port, which cannot provide a symbol the core defines.
  if(ARGC GREATER 1)
    target_link_libraries(${name} PRIVATE ${ARGN})
  else()
    target_link_libraries(${name} PRIVATE r2s_test_support)
  endif()
  # One ctest entry per test function, read at configure time; editing the
  # source re-runs the configure step.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${R2S_RUNNER_TOOL}" names "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    OUTPUT_VARIABLE test_functions OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  string(REPLACE "\n" ";" test_functions "${test_functions}")
  foreach(fn ${test_functions})
    add_test(NAME ${name}.${fn} COMMAND ${name} ${fn})
  endforeach()
endfunction()

# r2s_vector_test(<name> <kind> <check.c> <yaml>...): runs every case of the
# files through the kind's check function in <check.c>. Paths are passed
# relative to the repository, so failure messages name the vector file.
function(r2s_vector_test name kind check)
  set(files)
  foreach(f ${ARGN})
    file(RELATIVE_PATH rel "${R2S_REPO_ROOT}" "${f}")
    list(APPEND files "${rel}")
  endforeach()
  if(NOT files)
    return()
  endif()
  set(runner "${CMAKE_CURRENT_BINARY_DIR}/${name}_runner.c")
  add_custom_command(
    OUTPUT "${runner}"
    COMMAND Python3::Interpreter "${R2S_RUNNER_TOOL}" vectors ${kind} "${CMAKE_CURRENT_SOURCE_DIR}/${check}"
            "${runner}" ${files}
    WORKING_DIRECTORY "${R2S_REPO_ROOT}"
    DEPENDS ${ARGN} "${R2S_RUNNER_TOOL}"
    VERBATIM)
  add_executable(${name} "${check}" "${runner}")
  target_compile_options(${name} PRIVATE ${R2S_TEST_WARNINGS})
  target_link_libraries(${name} PRIVATE r2s_test_support)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

file(GLOB unit_tests CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" test_*.c)
# The ring has its own build (r2s_rx_ring), so its test is registered below
# with that library instead of the shared support.
list(REMOVE_ITEM unit_tests test_rx_ring.c)
foreach(source ${unit_tests})
  r2s_unit_test(${source})
endforeach()

add_executable(cxx_headers cxx_headers.cpp)
target_link_libraries(cxx_headers PRIVATE r2s_test_support)
target_compile_options(cxx_headers PRIVATE -Wall -Wextra -Werror)
add_test(NAME cxx_headers COMMAND cxx_headers)

r2s_unit_test(test_rx_ring.c unity r2s_rx_ring)

# Proof that a failing assertion fails: without these, a check that silently
# returned early would make every test pass.
r2s_unit_test(must_fail/bytes_mismatch.c)
set_tests_properties(bytes_mismatch.test_must_fail_assert_bytes_reports_a_mismatch PROPERTIES
  PASS_REGULAR_EXPRESSION "bytes_mismatch\\.c:[0-9]+:test_must_fail_assert_bytes_reports_a_mismatch:FAIL: demo: expected \\[01 02\\] \\(2 bytes\\), got \\[01 03 04\\] \\(3 bytes\\)")
r2s_unit_test(must_fail/overrun_unnoticed.c)
set_tests_properties(overrun_unnoticed.test_must_fail_an_unnoticed_overrun_fails_the_test PROPERTIES PASS_REGULAR_EXPRESSION "core wrote past tx_free")
r2s_unit_test(must_fail/selftest_mismatch.c)
target_sources(selftest_mismatch PRIVATE support/vector_selftest.c)
set_tests_properties(selftest_mismatch.test_must_fail_selftest_rejects_mistranscribed_bytes PROPERTIES
  PASS_REGULAR_EXPRESSION "must_fail case 0 \\[COB-1\\] decoded: expected \\[11 22\\]")

# The self-test runs the fixtures and every published vector of each kind
# through support/vector_selftest.c, so the transcription is checked before
# the modules (C2 to C4) register their own checks for the same files.
set(R2S_VECTOR_KINDS crc frames rcobs varint)
foreach(kind ${R2S_VECTOR_KINDS})
  file(GLOB fixtures CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/vectors/${kind}/*.yaml")
  file(GLOB published CONFIGURE_DEPENDS "${R2S_REPO_ROOT}/spec/vectors/${kind}/*.yaml")
  r2s_vector_test(vectors_selftest_fixtures_${kind} ${kind} support/vector_selftest.c ${fixtures})
  r2s_vector_test(vectors_selftest_${kind} ${kind} support/vector_selftest.c ${published})
endforeach()

# Conformance: the published vectors through each module that implements a kind.
file(GLOB crc_vectors CONFIGURE_DEPENDS "${R2S_REPO_ROOT}/spec/vectors/crc/*.yaml")
r2s_vector_test(vectors_crc crc vectors/crc_check.c ${crc_vectors})

file(GLOB varint_vectors CONFIGURE_DEPENDS "${R2S_REPO_ROOT}/spec/vectors/varint/*.yaml")
r2s_vector_test(vectors_varint varint vectors/varint_check.c ${varint_vectors})

file(GLOB rcobs_vectors CONFIGURE_DEPENDS "${R2S_REPO_ROOT}/spec/vectors/rcobs/*.yaml")
r2s_vector_test(vectors_rcobs rcobs vectors/rcobs_check.c ${rcobs_vectors})

file(GLOB frames_vectors CONFIGURE_DEPENDS "${R2S_REPO_ROOT}/spec/vectors/frames/*.yaml")
r2s_vector_test(vectors_frames frames vectors/frames_check.c ${frames_vectors})

# Vector kinds the C runner does not read yet, each with the ticket that adds
# it. A kind with files that is in neither list would be silently skipped, so
# it stops the configure step instead.
set(R2S_VECTOR_KINDS_PENDING messages:C9 session:C8 time:S7)
file(GLOB kind_dirs LIST_DIRECTORIES true "${R2S_REPO_ROOT}/spec/vectors/*")
foreach(dir ${kind_dirs})
  get_filename_component(kind "${dir}" NAME)
  file(GLOB files "${dir}/*.yaml")
  if(NOT IS_DIRECTORY "${dir}" OR NOT files OR kind IN_LIST R2S_VECTOR_KINDS)
    continue()
  endif()
  set(ticket "")
  foreach(entry ${R2S_VECTOR_KINDS_PENDING})
    string(REPLACE ":" ";" pair "${entry}")
    list(GET pair 0 pending_kind)
    if(pending_kind STREQUAL kind)
      list(GET pair 1 ticket)
    endif()
  endforeach()
  if(ticket)
    message(STATUS "spec/vectors/${kind}: not run by the C core until ${ticket}")
  else()
    message(FATAL_ERROR "spec/vectors/${kind} has vectors but no C check; add one or list the kind as pending")
  endif()
endforeach()
