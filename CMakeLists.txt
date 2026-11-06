# bbtree++: a C++20 port of bbtree (https://github.com/XiangpengHao/bbtree).
#
# The library (`bbtree/`) has no dependencies. Tests fetch Catch2; the bench
# fetches abseil, ankerl::unordered_dense and mimalloc (PORTING.md, P12).
cmake_minimum_required(VERSION 3.25)
project(bbtree++ VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BBTREE_BUILD_TESTS "Build the tests (fetches Catch2)" ${PROJECT_IS_TOP_LEVEL})
option(BBTREE_BUILD_BENCH "Build the bench (fetches abseil, unordered_dense, mimalloc)" OFF)
option(BBTREE_BUILD_FUZZ "Build the libFuzzer targets (Clang only)" OFF)
# The structural checks (`check_invariants`), as the Rust crate's
# `cfg(test)`/`cfg(fuzzing)` compile them: tests and fuzz targets turn them on.
option(BBTREE_CHECKED "Compile the trees' structural invariant checks" ${BBTREE_BUILD_TESTS})
option(BBTREE_NATIVE "Build for the host CPU (-march=native)" OFF)
set(BBTREE_SANITIZE "" CACHE STRING "Sanitizers: address,undefined | thread | empty")

add_library(bbtree_warnings INTERFACE)
target_compile_options(bbtree_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wnull-dereference
  -Wimplicit-fallthrough -Werror)

if(BBTREE_NATIVE)
  add_compile_options(-march=native)
endif()

if(BBTREE_SANITIZE)
  add_compile_options(-fsanitize=${BBTREE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${BBTREE_SANITIZE})
  if(BBTREE_SANITIZE MATCHES "thread")
    add_compile_definitions(BBTREE_TSAN=1)
  endif()
endif()

if(BBTREE_BUILD_TESTS)
  enable_testing()
  include(cmake/Catch2.cmake)
endif()

add_subdirectory(bbtree)
