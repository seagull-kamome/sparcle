cmake_minimum_required(VERSION 3.0 FATAL_ERROR)


project(sparcle C CXX)

set (SPARCLE_VERSION_MAJOR 0)
set (SPARCLE_VERSION_MINOR 0)

configure_file(
  "${PROJECT_SOURCE_DIR}/src/sparcle.h.in"
  "${PROJECT_BINARY_DIR}/sparcle.h")
include_directories("${PROJECT_BINARY_DIR}")


add_library(sparcle
  src/sparcle.c)
set (CMAKE_CXX_COMPILER gcc)
set (CMAKE_C_COMPILER gcc)
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu1z")
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11")


