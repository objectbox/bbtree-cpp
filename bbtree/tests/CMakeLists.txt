# The library's tests. They build twice: for the baseline target (on x86-64,
# which counts inline keys with SSE2 masks, no POPCNT) and for the host CPU,
# where the compiler vectorizes the count, so both paths are tested.
set(BBTREE_TEST_SOURCES
  allocator_test.cpp
  pooled_allocator_test.cpp
  file_test.cpp
  file_format_test.cpp
  fuzz_harness_test.cpp
  readme_test.cpp
  node/inline_key_test.cpp
  node/layout_test.cpp
  tree/differential_test.cpp
  unsync/map_api_test.cpp
  unsync/iterator_differential_test.cpp
  unsync/set_test.cpp
  unsync/batch_test.cpp
  unsync/compact_string_test.cpp
  unsync/move_only_test.cpp
  unsync/std_map_expectations_test.cpp
  unsync/drop_accounting_test.cpp
  unsync/bulk_drop_accounting_test.cpp
  unsync/set_drop_accounting_test.cpp
  unsync/page_ownership_test.cpp
  unsync/exception_safety_test.cpp
  unsync/lying_keys_test.cpp
  shared/lookup_stress_test.cpp
  shared/change_stress_test.cpp
  shared/scan_test.cpp
  shared/annex_test.cpp)

find_package(Threads REQUIRED)

function(bbtree_add_tests target)
  add_executable(${target} ${BBTREE_TEST_SOURCES})
  target_link_libraries(${target} PRIVATE bbtree::bbtree bbtree_warnings Catch2::Catch2WithMain Threads::Threads)
  catch_discover_tests(${target} DISCOVERY_MODE PRE_TEST TEST_PREFIX "${target}: ")
endfunction()

bbtree_add_tests(bbtree_tests)
if(NOT BBTREE_NATIVE)
  bbtree_add_tests(bbtree_tests_native)
  target_compile_options(bbtree_tests_native PRIVATE ${BBTREE_NATIVE_FLAG})
endif()

# The shared map's threads under a scheduler of their own
# (support/interleaving_schedule.hpp): the one target built with the latched
# tree's switch points, which every other target compiles to nothing.
add_executable(bbtree_interleaving_tests
  support/interleaving_schedule.cpp
  shared/interleaving_schedule_test.cpp
  shared/interleaving_test.cpp)
target_compile_definitions(bbtree_interleaving_tests PRIVATE BBTREE_INTERLEAVING=1)
target_link_libraries(bbtree_interleaving_tests PRIVATE bbtree::bbtree bbtree_warnings Catch2::Catch2WithMain
                                                        Threads::Threads)
catch_discover_tests(bbtree_interleaving_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "bbtree_interleaving_tests: ")
