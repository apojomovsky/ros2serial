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

# Generate firmware sources from r2s.yaml during the build.
#
#   r2s_generate_firmware(CONFIG <r2s.yaml> [MSG_PATHS <dir>...] [OUTPUT_DIR <dir>])
#
# Runs `r2s-gen generate --layout cmake` on the config at build time and
# writes r2s_generated.h, r2s_generated.c and CMakeLists.txt under OUTPUT_DIR.
# CONFIG is required; a relative path reads from the caller's source directory.
# Each MSG_PATHS entry is a directory of message packages, passed verbatim as
# --msg-path; a relative entry reads from the caller's source directory. The
# default is the caller's parent directory when the caller has a msg/
# directory, so a package's own messages resolve without spelling it out, and
# is empty otherwise, so a package with only standard messages passes no
# extra search path. OUTPUT_DIR defaults to `r2s_generated` under the caller's
# build directory. The driving target is named after OUTPUT_DIR's last
# component. A bad config fails the build with r2s-gen's own message.
function(r2s_generate_firmware)
  cmake_parse_arguments(ARG "" "CONFIG;OUTPUT_DIR" "MSG_PATHS" ${ARGN})
  if(NOT ARG_CONFIG)
    message(FATAL_ERROR "r2s_generate_firmware requires CONFIG <r2s.yaml>")
  endif()
  if(IS_ABSOLUTE "${ARG_CONFIG}")
    set(_config "${ARG_CONFIG}")
  else()
    set(_config "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_CONFIG}")
  endif()
  if(NOT EXISTS "${_config}")
    message(FATAL_ERROR "r2s_generate_firmware found no config at ${_config}")
  endif()

  if(ARG_OUTPUT_DIR)
    set(_out "${ARG_OUTPUT_DIR}")
  else()
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/r2s_generated")
  endif()
  if(NOT IS_ABSOLUTE "${_out}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${_out}")
  endif()

  if(ARG_MSG_PATHS)
    set(_dirs ${ARG_MSG_PATHS})
  elseif(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/msg")
    get_filename_component(_parent "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
    set(_dirs "${_parent}")
  endif()
  set(_paths "")
  set(_watched "")
  foreach(_dir IN LISTS _dirs)
    if(IS_ABSOLUTE "${_dir}")
      set(_abs "${_dir}")
    else()
      set(_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}")
    endif()
    if(NOT IS_DIRECTORY "${_abs}")
      message(FATAL_ERROR "r2s_generate_firmware found no message directory at ${_abs}")
    endif()
    list(APPEND _paths "--msg-path" "${_abs}")
    file(GLOB_RECURSE _found CONFIGURE_DEPENDS "${_abs}/*.msg")
    list(APPEND _watched ${_found})
  endforeach()

  # The workspace's installed generator first, then the underlay and PATH:
  # ament_python stages the script under lib/<package>, off the default search.
  if(NOT R2S_GEN_EXECUTABLE)
    set(_prefixes ${CMAKE_PREFIX_PATH})
    if(DEFINED ENV{AMENT_PREFIX_PATH})
      string(REPLACE ":" ";" _env_prefixes "$ENV{AMENT_PREFIX_PATH}")
      list(APPEND _prefixes ${_env_prefixes})
    endif()
    find_program(
      R2S_GEN_EXECUTABLE
      NAMES r2s-gen
      HINTS ${_prefixes}
      PATH_SUFFIXES lib/r2s_gen bin)
  endif()
  if(NOT R2S_GEN_EXECUTABLE)
    message(FATAL_ERROR "r2s_generate_firmware found no r2s-gen program (build the r2s_gen package)")
  endif()

  # The build interpreter runs the script, not its baked shebang: colcon
  # installs the script for the Python that runs colcon itself, which may not
  # see the generator's dependencies, while the build environment does.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  set(_outputs "${_out}/r2s_generated.h" "${_out}/r2s_generated.c" "${_out}/CMakeLists.txt")
  add_custom_command(
    OUTPUT ${_outputs}
    COMMAND ${Python3_EXECUTABLE} ${R2S_GEN_EXECUTABLE} generate --config "${_config}" --out "${_out}" --layout cmake ${_paths}
    DEPENDS "${_config}" ${_watched}
    COMMENT "Generating firmware sources from ${_config}"
    VERBATIM)
  get_filename_component(_target "${_out}" NAME)
  string(MAKE_C_IDENTIFIER "${_target}" _target)
  add_custom_target(${_target} ALL DEPENDS ${_outputs})
endfunction()
