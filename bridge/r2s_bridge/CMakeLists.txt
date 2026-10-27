cmake_minimum_required(VERSION 3.16)
project(r2s_bridge)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
if(NOT CMAKE_CXX_STANDARD_REQUIRED)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_components REQUIRED)

add_library(r2s_bridge_component SHARED
  src/bridge.cpp
  src/core_port.cpp
  src/framing.cpp
  src/session.cpp
  src/termios_transport.cpp)
target_include_directories(r2s_bridge_component PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(r2s_bridge_component PUBLIC
  rclcpp::rclcpp
  rclcpp_components::component)
target_compile_options(r2s_bridge_component PRIVATE -Wall -Wextra -Wpedantic -Werror)

# The core's framer, rCOBS and CRC compiled for the host, so the bridge and
# the MCU share one framing implementation (docs/00-design.md, Code reuse
# between MCU and host). The host's maximum decoded frame is 65277 and is not
# announced (FRM-10).
add_library(r2s_core_framing STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src/crc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src/framer.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src/rcobs.c)
target_include_directories(r2s_core_framing PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include)
set_target_properties(r2s_core_framing PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(r2s_core_framing PUBLIC R2S_MAX_FRAME=65277)
target_compile_options(r2s_core_framing PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(r2s_bridge_component PRIVATE r2s_core_framing)
rclcpp_components_register_nodes(r2s_bridge_component "r2s_bridge::Bridge")

add_executable(bridge src/main.cpp)
target_link_libraries(bridge PRIVATE r2s_bridge_component)
target_compile_options(bridge PRIVATE -Wall -Wextra -Wpedantic -Werror)

install(TARGETS r2s_bridge_component
  EXPORT export_r2s_bridge
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)
install(TARGETS bridge RUNTIME DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY include/ DESTINATION include)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(class_loader REQUIRED)
  find_package(yaml-cpp REQUIRED)

  set(R2S_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../core)

  # The fakes are shared by every bridge test, so they build once instead of
  # being copied into each test file.
  add_library(r2s_bridge_test_support STATIC
    test/support/fake_transport.cpp
    test/support/manual_clock.cpp)
  target_include_directories(r2s_bridge_test_support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/test/support>)
  target_link_libraries(r2s_bridge_test_support PUBLIC r2s_bridge_component)
  target_compile_options(r2s_bridge_test_support PRIVATE -Wall -Wextra -Wpedantic -Werror)

  ament_add_gtest(test_parameters test/test_parameters.cpp)
  target_link_libraries(test_parameters r2s_bridge_component)

  ament_add_gtest(test_component_load test/test_component_load.cpp)
  target_compile_definitions(test_component_load PRIVATE
    R2S_BRIDGE_COMPONENT_LIBRARY="$<TARGET_FILE:r2s_bridge_component>")
  target_link_libraries(test_component_load
    class_loader::class_loader
    rclcpp_components::component
    rclcpp::rclcpp)

  ament_add_gtest(test_fake_transport test/test_fake_transport.cpp)
  target_link_libraries(test_fake_transport r2s_bridge_test_support)

  ament_add_gtest(test_manual_clock test/test_manual_clock.cpp)
  target_link_libraries(test_manual_clock r2s_bridge_test_support)

  ament_add_gtest(test_session_without_executor test/test_session_without_executor.cpp)
  target_link_libraries(test_session_without_executor r2s_bridge_test_support)

  # The frame vectors live at the repository root; the framing test feeds each
  # one byte by byte through the linked core framer and compares the frame.
  ament_add_gtest(test_framing test/test_framing.cpp)
  target_include_directories(test_framing PRIVATE ${R2S_CORE_DIR}/include)
  target_compile_definitions(test_framing PRIVATE
    R2S_FRAMES_VECTOR_DIR="${R2S_CORE_DIR}/../spec/vectors/frames")
  target_link_libraries(test_framing r2s_bridge_test_support yaml-cpp::yaml-cpp)

  # The session reads the transcript vectors and drives the fake link and
  # the manual clock.
  ament_add_gtest(test_session test/test_session.cpp)
  target_include_directories(test_session PRIVATE ${R2S_CORE_DIR}/include)
  target_compile_definitions(test_session PRIVATE
    R2S_SESSION_VECTOR_DIR="${R2S_CORE_DIR}/../spec/vectors/session"
    R2S_FRAMES_VECTOR_DIR="${R2S_CORE_DIR}/../spec/vectors/frames")
  target_link_libraries(test_session r2s_bridge_test_support yaml-cpp::yaml-cpp)

  # openpty comes from libutil; the pty test uses it instead of a device.
  ament_add_gtest(test_termios_transport test/test_termios_transport.cpp)
  target_link_libraries(test_termios_transport r2s_bridge_component util)
endif()

ament_export_targets(export_r2s_bridge HAS_LIBRARY_TARGET)
ament_export_dependencies(rclcpp rclcpp_components)
ament_package()
