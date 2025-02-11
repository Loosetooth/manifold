# Copyright 2020 The Manifold Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.18)
project(manifold LANGUAGES CXX)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MANIFOLD_EXPORT "Build mesh export (via assimp) utility library" OFF)
option(MANIFOLD_TEST "Enable testing suite" ON)
option(MANIFOLD_DEBUG "Enable debug tracing/timing" OFF)
option(MANIFOLD_PYBIND "Build python bindings" ON)
option(MANIFOLD_CBIND "Build C (FFI) bindings" OFF)
option(TRACY_ENABLE "Use tracy profiling" OFF) 
option(TRACY_MEMORY_USAGE "Track memory allocation with tracy (expensive)" OFF)

if(TRACY_ENABLE)
  option(CMAKE_BUILD_TYPE "Build type" RelWithDebInfo)
  if(TRACY_MEMORY_USAGE)
    set(MANIFOLD_FLAGS ${MANIFOLD_FLAGS} -DTRACY_MEMORY_USAGE)
  endif()
  if(NOT MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer")
  endif()
else()
  option(CMAKE_BUILD_TYPE "Build type" Release)
endif()

set(MANIFOLD_PAR "NONE" CACHE STRING "Parallel backend, either \"TBB\" or \"NONE\"")
set(MANIFOLD_FLAGS ${MANIFOLD_FLAGS} -O3)

if(EMSCRIPTEN)
  message("Building for Emscripten")
  set(MANIFOLD_FLAGS -fexceptions -D_LIBCUDACXX_HAS_THREAD_API_EXTERNAL -D_LIBCUDACXX_HAS_THREAD_API_CUDA)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -sALLOW_MEMORY_GROWTH=1 -fexceptions -sDISABLE_EXCEPTION_CATCHING=0")
  set(MANIFOLD_PYBIND OFF)
endif()

if(MANIFOLD_PYBIND)
find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
endif()

option(BUILD_TEST_CGAL "Build CGAL performance comparisons" OFF)

option(BUILD_SHARED_LIBS "Build dynamic/shared libraries" OFF)
set(PYBIND11_DIR ${PROJECT_SOURCE_DIR}/bindings/python/third_party/pybind11)
set(THRUST_INC_DIR ${PROJECT_SOURCE_DIR}/src/third_party/thrust)

if(CMAKE_EXPORT_COMPILE_COMMANDS AND !EMSCRIPTEN)
  # for nixos
  set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES 
      ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

if(NOT MSVC)
  if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(WARNING_FLAGS -Werror -Wall -Wno-sign-compare -Wno-unused -Wno-array-bounds -Wno-stringop-overflow)
  else()
    set(WARNING_FLAGS -Werror -Wall -Wno-sign-compare -Wno-unused)
  endif()
  set(MANIFOLD_FLAGS
    "${MANIFOLD_FLAGS}"
    "$<$<COMPILE_LANGUAGE:CXX>:${WARNING_FLAGS}>")
endif()

if(CODE_COVERAGE AND NOT MSVC)
  set(COVERAGE_FLAGS -coverage -fno-inline-small-functions -fkeep-inline-functions -fkeep-static-functions)
  set(MANIFOLD_FLAGS
    "${MANIFOLD_FLAGS}"
    "$<$<COMPILE_LANGUAGE:CXX>:${COVERAGE_FLAGS}>")
  add_link_options("-coverage")
endif()

add_subdirectory(src)
add_subdirectory(bindings)

if(MANIFOLD_EXPORT)
  add_subdirectory(meshIO)
endif()

if(MANIFOLD_TEST)
  add_subdirectory(samples)
  add_subdirectory(test)
  add_subdirectory(extras)
endif()
