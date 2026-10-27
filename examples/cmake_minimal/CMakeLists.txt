# Example firmware: the generated static library plus the ATmega328P main.
cmake_minimum_required(VERSION 3.22)
project(hello_cmake C)

# The in-tree core checkout; an out-of-tree copy passes its own -DR2S_CORE_DIR.
if(NOT R2S_CORE_DIR)
  set(R2S_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../core")
endif()
add_subdirectory(lib)

add_executable(hello_cmake main.c)
target_link_libraries(hello_cmake PRIVATE r2s_generated)
