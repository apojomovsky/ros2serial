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
find_package(rmw REQUIRED)
find_package(rosidl_runtime_cpp REQUIRED)
find_package(rosidl_typesupport_introspection_cpp REQUIRED)

add_library(r2s_bridge_component SHARED
  src/bridge.cpp
  src/converter.cpp
  src/core_port.cpp
  src/framing.cpp
  src/router.cpp
  src/scheduler.cpp
  src/session.cpp
  src/termios_transport.cpp
  src/type_support.cpp)
target_include_directories(r2s_bridge_component PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(r2s_bridge_component PUBLIC
  rclcpp::rclcpp
  rclcpp_components::component
  rmw::rmw
  rosidl_runtime_cpp::rosidl_runtime_cpp
  rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp)
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

# The launch file and the example parameters install into the package share,
# so the launch test and the e2e framework find them without the sources.
install(DIRECTORY launch config DESTINATION share/${PROJECT_NAME})

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

  # The scheduler is plain C++ over a nanosecond clock, so its tests need no
  # ROS context.
  ament_add_gtest(test_scheduler test/test_scheduler.cpp)
  target_link_libraries(test_scheduler r2s_bridge_test_support)

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

  # The type support test loads the installed message packages for real, so
  # it needs them at test time (std_msgs) as well as at build time.
  ament_add_gtest(test_type_support test/test_type_support.cpp)
  target_link_libraries(test_type_support r2s_bridge_test_support)

  # The converter test loads the installed message packages for real and
  # compares every message vector with rclcpp serialization, so it needs the
  # message C++ headers at build time and the packages at test time.
  find_package(geometry_msgs REQUIRED)
  find_package(rcl_interfaces REQUIRED)
  find_package(sensor_msgs REQUIRED)
  find_package(std_msgs REQUIRED)
  ament_add_gtest(test_converter test/test_converter.cpp)
  target_compile_definitions(test_converter PRIVATE
    R2S_MESSAGES_VECTOR_DIR="${R2S_CORE_DIR}/../spec/vectors/messages")
  target_link_libraries(test_converter
    r2s_bridge_component
    yaml-cpp::yaml-cpp
    geometry_msgs::geometry_msgs__rosidl_typesupport_cpp
    rcl_interfaces::rcl_interfaces__rosidl_typesupport_cpp
    sensor_msgs::sensor_msgs__rosidl_typesupport_cpp
    std_msgs::std_msgs__rosidl_typesupport_cpp)

  # openpty comes from libutil; the pty test uses it instead of a device.
  ament_add_gtest(test_termios_transport test/test_termios_transport.cpp)
  target_link_libraries(test_termios_transport r2s_bridge_component util)

  # The router drives a real node with a scripted link and real type support,
  # covering remapping, QoS and both traffic directions end to end.
  ament_add_gtest(test_router test/test_router.cpp)
  target_include_directories(test_router PRIVATE ${R2S_CORE_DIR}/include)
  target_link_libraries(test_router
    r2s_bridge_component
    r2s_bridge_test_support
    geometry_msgs::geometry_msgs__rosidl_typesupport_cpp
    sensor_msgs::sensor_msgs__rosidl_typesupport_cpp
    std_msgs::std_msgs__rosidl_typesupport_cpp)
endif()

ament_export_targets(export_r2s_bridge HAS_LIBRARY_TARGET)
ament_export_dependencies(rclcpp rclcpp_components)
ament_package()
