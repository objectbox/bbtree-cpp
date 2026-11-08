# The bench harness, a port of the Rust crate's `bbtree/bench` (PORTING.md,
# P12, P17-P19). Three binaries share every source but the one that fixes the
# global allocator: `bbtree-bench` times cells on plain mimalloc,
# `bbtree-bench-memory` counts the live bytes of the maps instead, and
# `bbtree-bench-system` times cells on the system's allocator.
#
# The bench-only dependencies, pinned to releases; the library stays
# dependency-free. `SYSTEM` keeps their headers out of the bench's warnings,
# and `EXCLUDE_FROM_ALL` builds only the parts the bench links.
include(FetchContent)

set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(abseil
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG 20250814.2
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)

FetchContent_Declare(unordered_dense
  GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
  GIT_TAG v4.9.2
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)

# mimalloc 3.3.2: the version the Rust bench's libmimalloc-sys 0.1.49 builds
# by default (its `v2` feature is off), with its defaults, so that both heaps
# back their arenas with the same transparent huge pages (PORTING.md, P17). It
# replaces `operator new` and `delete` through `mimalloc-new-delete.h` in the
# timing binary: the C++ counterpart of the Rust bench's `#[global_allocator]`.
# It does not interpose `malloc`.
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(mimalloc
  GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
  GIT_TAG v3.3.2
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(abseil unordered_dense mimalloc)

find_package(Threads REQUIRED)

# Everything but the allocator, compiled once for both binaries.
add_library(bbtree_bench_core STATIC
  src/bench_program.cpp
  src/cell_runner.cpp
  src/child_process.cpp
  src/command_line.cpp
  src/datasets.cpp
  src/dataset_fetch.cpp
  src/json_objects.cpp
  src/key_list.cpp
  src/keys.cpp
  src/map_kind.cpp
  src/memory.cpp
  src/perf_counters.cpp
  src/report.cpp
  src/run_comparison.cpp
  src/self_check.cpp
  src/sha256.cpp
  src/suite_runner.cpp
  src/suites.cpp
  src/workload.cpp)
target_link_libraries(bbtree_bench_core
  PUBLIC bbtree::bbtree absl::btree unordered_dense::unordered_dense mimalloc-static Threads::Threads
  PRIVATE bbtree_warnings)
# stx_btree, another C++ form of bbtree, as the opt-in map `stx-unsync`:
# a path to a checkout of it (CPP_MAPS.md, against stx_btree).
set(BBTREE_BENCH_STX_DIR "" CACHE PATH "A checkout of stx_btree, for the bench's stx-unsync map")
if(BBTREE_BENCH_STX_DIR)
  target_include_directories(bbtree_bench_core SYSTEM PUBLIC ${BBTREE_BENCH_STX_DIR}/include)
  target_compile_definitions(bbtree_bench_core PUBLIC BBTREE_BENCH_STX=1)
endif()
target_compile_definitions(bbtree_bench_core PRIVATE
  BBTREE_BENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  BBTREE_REPOSITORY_DIR="${PROJECT_SOURCE_DIR}")

add_executable(bbtree-bench src/timing_main.cpp)
target_link_libraries(bbtree-bench PRIVATE bbtree_bench_core bbtree_warnings)

add_executable(bbtree-bench-memory src/memory_main.cpp)
target_link_libraries(bbtree-bench-memory PRIVATE bbtree_bench_core bbtree_warnings)

add_executable(bbtree-bench-system src/system_allocator_main.cpp)
target_link_libraries(bbtree-bench-system PRIVATE bbtree_bench_core bbtree_warnings)
