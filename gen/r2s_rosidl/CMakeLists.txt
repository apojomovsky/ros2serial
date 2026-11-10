# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

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
