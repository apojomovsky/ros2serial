# The colcon-time wrapper over r2s-gen: install the CMake helper a firmware
# package finds to generate its sources during the build.
cmake_minimum_required(VERSION 3.22)
project(r2s_rosidl)

find_package(ament_cmake REQUIRED)

if(BUILD_TESTING)
  # The ament linters stay advisory: the ament-lint CI job reports their
  # findings and never fails this build, until the cleanup tickets land.
  option(R2S_AMENT_LINT "Run the ament linters (advisory, see #508)" OFF)
  if(R2S_AMENT_LINT)
    find_package(ament_lint_auto REQUIRED)
    ament_lint_auto_find_test_dependencies()
  endif()
endif()

install(DIRECTORY cmake/ DESTINATION share/${PROJECT_NAME}/cmake)

ament_package(CONFIG_EXTRAS cmake/r2s_rosidl-extras.cmake)
