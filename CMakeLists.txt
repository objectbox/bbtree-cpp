# bbtree++: a C++20 port of bbtree (https://github.com/XiangpengHao/bbtree).
#
# The library (`bbtree/`) has no dependencies. Tests fetch Catch2; the bench
# fetches abseil, ankerl::unordered_dense and mimalloc (PORTING.md, P12).
cmake_minimum_required(VERSION 3.28)
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
option(BBTREE_NATIVE "Build for the host CPU (-march=native, AArch64: -mcpu=native)" OFF)
option(BBTREE_INSTALL "Generate the install rules and the bbtree CMake package" ${PROJECT_IS_TOP_LEVEL})

include(GNUInstallDirs)
set(BBTREE_SANITIZE "" CACHE STRING "Sanitizers: address,undefined | thread | empty")

add_library(bbtree_warnings INTERFACE)
target_compile_options(bbtree_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Wimplicit-fallthrough -Werror
  # GCC's -Wnull-dereference flags optimized paths it can't prove unreachable,
  # in libstdc++'s std::string too: Clang's only.
  $<$<CXX_COMPILER_ID:Clang>:-Wnull-dereference>)

# The host CPU's flag: x86 names it by architecture, AArch64 by CPU.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(BBTREE_NATIVE_FLAG -mcpu=native)
else()
  set(BBTREE_NATIVE_FLAG -march=native)
endif()
# For some hosts' CPUs Clang's own `-march=native` names a feature set it
# then warns of (`-Winvalid-feature-combination`: a 256-bit AVX10.1 it
# promotes to 512 bits), which `-Werror` would make an error of every native
# build there. The check fails for a compiler that doesn't know the warning.
include(CheckCXXCompilerFlag)
set(CMAKE_REQUIRED_FLAGS -Werror)
check_cxx_compiler_flag(-Winvalid-feature-combination BBTREE_HAS_INVALID_FEATURE_COMBINATION_WARNING)
unset(CMAKE_REQUIRED_FLAGS)
if(BBTREE_HAS_INVALID_FEATURE_COMBINATION_WARNING)
  list(APPEND BBTREE_NATIVE_FLAG -Wno-error=invalid-feature-combination)
endif()
if(BBTREE_NATIVE)
  add_compile_options(${BBTREE_NATIVE_FLAG})
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
add_subdirectory(bbtree-mmap)
add_subdirectory(bbtree-store)
if(BBTREE_BUILD_FUZZ)
  add_subdirectory(bbtree/fuzz)
endif()
if(BBTREE_BUILD_BENCH)
  add_subdirectory(bbtree/bench)
endif()

# `cmake --install`: the headers of the three libraries and the `bbtree`
# package, whose targets are named as `add_subdirectory` names them:
# `find_package(bbtree)`, then `bbtree::bbtree`, `bbtree::mmap` or
# `bbtree::store` (PORTING.md, P34).
if(BBTREE_INSTALL)
  include(CMakePackageConfigHelpers)
  install(TARGETS bbtree bbtree_mmap bbtree_store EXPORT bbtreeTargets)
  install(DIRECTORY bbtree/include/ bbtree-mmap/include/ bbtree-store/include/
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  set(BBTREE_PACKAGE_DIR ${CMAKE_INSTALL_DATADIR}/cmake/bbtree)
  install(EXPORT bbtreeTargets NAMESPACE bbtree:: DESTINATION ${BBTREE_PACKAGE_DIR})
  configure_package_config_file(cmake/bbtreeConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/bbtreeConfig.cmake
                                INSTALL_DESTINATION ${BBTREE_PACKAGE_DIR})
  # 0.x: a minor version may change the API, so only the same minor matches.
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/bbtreeConfigVersion.cmake
                                   COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bbtreeConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/bbtreeConfigVersion.cmake
          DESTINATION ${BBTREE_PACKAGE_DIR})
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/bbtree)
endif()
