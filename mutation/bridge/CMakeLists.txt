# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The bridge mutation harness: the ROS-free bridge classes (scheduler,
# time sync, framing and the core port slot) with the gtest binaries that
# drive them, so the nightly mutation run needs no ROS. The rclcpp shell
# around these classes has no harness yet. Built with the Mull frontend
# flags on the cmake command line, like the core in run-mutation.sh.
cmake_minimum_required(VERSION 3.16)
project(r2s_bridge_mutation_harness LANGUAGES C CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(R2S_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(R2S_BRIDGE_DIR "${R2S_REPO_ROOT}/bridge/r2s_bridge")
set(R2S_CORE_DIR "${R2S_REPO_ROOT}/core")

# The host framing configuration must match the bridge's own build
# (bridge/r2s_bridge/CMakeLists.txt): one process frames several links.
add_compile_definitions(R2S_MAX_FRAME=65277 R2S_MAX_LINKS=16)

find_package(GTest REQUIRED)
find_package(yaml-cpp REQUIRED)

add_library(r2s_core_framing STATIC
  "${R2S_CORE_DIR}/src/crc.c"
  "${R2S_CORE_DIR}/src/framer.c"
  "${R2S_CORE_DIR}/src/rcobs.c")
target_include_directories(r2s_core_framing PUBLIC "${R2S_CORE_DIR}/include")

add_library(r2s_bridge_under_test STATIC
  "${R2S_BRIDGE_DIR}/src/scheduler.cpp"
  "${R2S_BRIDGE_DIR}/src/time_sync.cpp"
  "${R2S_BRIDGE_DIR}/src/framing.cpp"
  "${R2S_BRIDGE_DIR}/src/core_port.cpp")
target_include_directories(r2s_bridge_under_test PUBLIC
  "${R2S_BRIDGE_DIR}/include"
  "${R2S_BRIDGE_DIR}/src"
  "${R2S_CORE_DIR}/include")
target_link_libraries(r2s_bridge_under_test PUBLIC r2s_core_framing)

add_library(r2s_bridge_test_support STATIC
  "${R2S_BRIDGE_DIR}/test/support/manual_clock.cpp"
  "${R2S_BRIDGE_DIR}/test/support/fake_transport.cpp")
target_include_directories(r2s_bridge_test_support PUBLIC
  "${R2S_BRIDGE_DIR}/include"
  "${R2S_BRIDGE_DIR}/test/support")
target_link_libraries(r2s_bridge_test_support PUBLIC r2s_bridge_under_test)

enable_testing()

add_executable(test_scheduler "${R2S_BRIDGE_DIR}/test/test_scheduler.cpp")
target_link_libraries(test_scheduler PRIVATE r2s_bridge_test_support GTest::gtest GTest::gtest_main)
add_test(NAME test_scheduler COMMAND test_scheduler)

add_executable(test_time_sync "${R2S_BRIDGE_DIR}/test/test_time_sync.cpp")
target_link_libraries(test_time_sync PRIVATE r2s_bridge_test_support GTest::gtest GTest::gtest_main)
add_test(NAME test_time_sync COMMAND test_time_sync)

add_executable(test_manual_clock "${R2S_BRIDGE_DIR}/test/test_manual_clock.cpp")
target_link_libraries(test_manual_clock PRIVATE r2s_bridge_test_support GTest::gtest GTest::gtest_main)
add_test(NAME test_manual_clock COMMAND test_manual_clock)

add_executable(test_fake_transport "${R2S_BRIDGE_DIR}/test/test_fake_transport.cpp")
target_link_libraries(test_fake_transport PRIVATE r2s_bridge_test_support GTest::gtest GTest::gtest_main)
add_test(NAME test_fake_transport COMMAND test_fake_transport)

add_executable(test_framing "${R2S_BRIDGE_DIR}/test/test_framing.cpp")
target_compile_definitions(test_framing PRIVATE
  R2S_FRAMES_VECTOR_DIR="${R2S_REPO_ROOT}/spec/vectors/frames")
target_link_libraries(test_framing PRIVATE r2s_bridge_test_support yaml-cpp::yaml-cpp GTest::gtest GTest::gtest_main)
add_test(NAME test_framing COMMAND test_framing)
