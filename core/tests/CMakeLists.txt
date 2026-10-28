# Unit tests (test_*.c, one executable each), must-fail tests (must_fail/*.c,
# which pass only by failing with the expected message) and vector tests (one
# executable per vector kind and check). Runners come from tools/unity_runner.py.

# Unity keeps its own style, so it builds without the core's warning flags.
add_library(unity STATIC vendor/unity/unity.c)
target_include_directories(unity SYSTEM PUBLIC vendor/unity)

# The ring is compiled only with R2S_WITH_RX_RING, so its tests build rx.c
# again with the switch on instead of changing the configuration every other
# test shares. One build per ring size: the header's default, and the smallest
# the header allows, which the default-size tests cannot reach.
function(r2s_ring_library target)
  add_library(${target} STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../src/rx.c")
  target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
  target_compile_definitions(${target} PUBLIC R2S_WITH_RX_RING)
  target_compile_options(${target} PRIVATE ${R2S_WARNINGS})
  if(ARGC GREATER 1)
    target_compile_definitions(${target} PUBLIC R2S_RX_RING_SIZE=${ARGV1})
  endif()
endfunction()

r2s_ring_library(r2s_rx_ring)
r2s_ring_library(r2s_rx_ring_min 2)

function(r2s_support_lib name core_lib)
  add_library(${name} STATIC support/bytes.c support/fake_port.c support/fake_port_unity.c
                             support/frames.c support/handwritten.c support/rcobs.c)
  target_include_directories(${name} PUBLIC support)
  target_compile_options(${name} PRIVATE ${R2S_WARNINGS})
  target_link_libraries(${name} PUBLIC unity ${core_lib})
  # Unity calls setUp and tearDown, so they are linked as an object, not from an archive.
  target_sources(${name} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support/hooks.c")
  # The default dispatch is linked the same way, as an object in every test
  # binary: an archive member would not be extracted for a symbol the core only
  # names later. The link tests override it with their strong definition.
  target_sources(${name} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support/dispatch_default.c")
endfunction()

r2s_support_lib(r2s_test_support r2s_core)

# The transcript firmware announces a 1 MHz tick without logging, while the
# host test build runs at 1 kHz with logging. The replay compiles the core
# again with the transcript's values, so its HELLO matches byte for byte. The
# -U drops the directory-wide R2S_WITH_LOG after its -D, which CMake emits
# first; both spellings are POSIX compiler options.
file(GLOB_RECURSE R2S_SESSION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../src/*.c")
add_library(r2s_core_session STATIC ${R2S_SESSION_SOURCES})
target_include_directories(r2s_core_session PUBLIC ../include)
target_compile_options(r2s_core_session PRIVATE ${R2S_WARNINGS})
target_compile_definitions(r2s_core_session PRIVATE R2S_TICK_HZ=1000000)
target_compile_options(r2s_core_session PRIVATE -U R2S_WITH_LOG)
r2s_support_lib(r2s_session_support r2s_core_session)

# The handwritten publish wrappers and dispatch, with the handler their tests
# provide: only the link tests need them, so they stay out of the shared
# support library.
add_library(r2s_handwritten_firmware STATIC support/handwritten_firmware.c)
target_include_directories(r2s_handwritten_firmware PUBLIC support)
target_compile_options(r2s_handwritten_firmware PRIVATE ${R2S_WARNINGS})
target_link_libraries(r2s_handwritten_firmware PUBLIC r2s_core)

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
# The ring tests have their own builds (r2s_rx_ring, r2s_rx_ring_min), so they
# are registered below with those libraries instead of the shared support.
# The dispatch test also links the handwritten firmware wiring, which carries
# the handler the test provides.
list(REMOVE_ITEM unit_tests test_rx_ring.c test_rx_ring_min.c test_codec_dispatch.c)
foreach(source ${unit_tests})
  r2s_unit_test(${source})
endforeach()
r2s_unit_test(test_codec_dispatch.c unity r2s_test_support r2s_handwritten_firmware)

add_executable(cxx_headers cxx_headers.cpp)
target_link_libraries(cxx_headers PRIVATE r2s_test_support)
target_compile_options(cxx_headers PRIVATE -Wall -Wextra -Werror)
add_test(NAME cxx_headers COMMAND cxx_headers)

# Power-up probe: its own main without the Unity reset, so it observes the
# BSS defaults the firmware boots with.
add_executable(powerup_hello powerup_hello.c)
target_compile_options(powerup_hello PRIVATE ${R2S_TEST_WARNINGS})
target_link_libraries(powerup_hello PRIVATE r2s_test_support)
add_test(NAME powerup_hello COMMAND powerup_hello)

r2s_unit_test(test_rx_ring.c unity r2s_rx_ring)
r2s_unit_test(test_rx_ring_min.c unity r2s_rx_ring_min)

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
set(R2S_VECTOR_KINDS crc frames messages rcobs session varint)
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

file(GLOB messages_vectors CONFIGURE_DEPENDS "${R2S_REPO_ROOT}/spec/vectors/messages/*.yaml")
r2s_vector_test(vectors_messages messages vectors/messages_check.c ${messages_vectors})

# The replay runs the transcripts the conforming session passes byte for
# byte. Loss and reset need harness reenactment: a lost frame is not fed, an
# expired link is fired early, a reboot reconfigures with the announced table.
# The reenactment lives in vectors/session_check.c, next to each transcript.
# The replay links the session build above, whose tick the transcripts pin, so
# it cannot use the shared support library and registers by hand.
set(R2S_SESSION_REPLAY_NAMES boot_handshake boot_wait_expiry empty_topics link_timeout pub_sub_handshake
                             reset_same_token reset_zero_token topic_retry topic_retry_exhausted)
set(R2S_SESSION_REPLAY_PATHS)
foreach(name ${R2S_SESSION_REPLAY_NAMES})
  set(path "${R2S_REPO_ROOT}/spec/vectors/session/${name}.yaml")
  file(RELATIVE_PATH rel "${R2S_REPO_ROOT}" "${path}")
  list(APPEND R2S_SESSION_REPLAY_FILES "${rel}")
  list(APPEND R2S_SESSION_REPLAY_PATHS "${path}")
endforeach()
set(session_runner "${CMAKE_CURRENT_BINARY_DIR}/vectors_session_runner.c")
add_custom_command(
  OUTPUT "${session_runner}"
  COMMAND Python3::Interpreter "${R2S_RUNNER_TOOL}" vectors session
          "${CMAKE_CURRENT_SOURCE_DIR}/vectors/session_check.c" "${session_runner}"
          ${R2S_SESSION_REPLAY_FILES}
  WORKING_DIRECTORY "${R2S_REPO_ROOT}"
  DEPENDS ${R2S_SESSION_REPLAY_PATHS} "${R2S_RUNNER_TOOL}"
  VERBATIM)
add_executable(vectors_session vectors/session_check.c "${session_runner}")
target_compile_options(vectors_session PRIVATE ${R2S_TEST_WARNINGS})
target_compile_definitions(vectors_session PRIVATE R2S_TICK_HZ=1000000)
target_link_libraries(vectors_session PRIVATE r2s_session_support)
add_test(NAME vectors_session COMMAND vectors_session)

# Every session transcript is either replayed above or names the follow-up
# that still keeps it out, so a new transcript stops the configure step
# instead of landing silently unrun.
set(R2S_SESSION_DIVERGENT_NAMES
  refused_hello
  version_mismatch)
file(GLOB R2S_SESSION_ALL_PATHS CONFIGURE_DEPENDS "${R2S_REPO_ROOT}/spec/vectors/session/*.yaml")
foreach(path ${R2S_SESSION_ALL_PATHS})
  get_filename_component(stem "${path}" NAME_WE)
  if(NOT stem IN_LIST R2S_SESSION_REPLAY_NAMES AND NOT stem IN_LIST R2S_SESSION_DIVERGENT_NAMES)
    message(FATAL_ERROR "spec/vectors/session/${stem}.yaml is neither replayed nor divergent: "
                        "add it to a list with the follow-up that takes it")
  endif()
endforeach()

# Vector kinds the C runner does not read yet, each with the ticket that adds
# it. A kind with files that is in neither list would be silently skipped, so
# it stops the configure step instead.
set(R2S_VECTOR_KINDS_PENDING time:S7)
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
