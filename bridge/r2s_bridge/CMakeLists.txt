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

add_library(r2s_bridge_component SHARED src/bridge.cpp)
target_include_directories(r2s_bridge_component PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(r2s_bridge_component PUBLIC
  rclcpp::rclcpp
  rclcpp_components::component)
target_compile_options(r2s_bridge_component PRIVATE -Wall -Wextra -Wpedantic -Werror)
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

  ament_add_gtest(test_parameters test/test_parameters.cpp)
  target_link_libraries(test_parameters r2s_bridge_component)

  ament_add_gtest(test_component_load test/test_component_load.cpp)
  target_compile_definitions(test_component_load PRIVATE
    R2S_BRIDGE_COMPONENT_LIBRARY="$<TARGET_FILE:r2s_bridge_component>")
  target_link_libraries(test_component_load
    class_loader::class_loader
    rclcpp_components::component
    rclcpp::rclcpp)
endif()

ament_export_targets(export_r2s_bridge HAS_LIBRARY_TARGET)
ament_export_dependencies(rclcpp rclcpp_components)
ament_package()
