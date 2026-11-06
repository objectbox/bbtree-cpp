# The library's tests. They build twice: for baseline x86-64, which counts
# inline keys with SSE2 masks (no POPCNT), and for the host CPU, where the
# compiler vectorizes the count, so both paths are tested.
set(BBTREE_TEST_SOURCES
  allocator_test.cpp
  node/inline_key_test.cpp
  node/layout_test.cpp
  tree/differential_test.cpp)

function(bbtree_add_tests target)
  add_executable(${target} ${BBTREE_TEST_SOURCES})
  target_link_libraries(${target} PRIVATE bbtree::bbtree bbtree_warnings Catch2::Catch2WithMain)
  catch_discover_tests(${target} DISCOVERY_MODE PRE_TEST TEST_PREFIX "${target}: ")
endfunction()

bbtree_add_tests(bbtree_tests)
if(NOT BBTREE_NATIVE)
  bbtree_add_tests(bbtree_tests_native)
  target_compile_options(bbtree_tests_native PRIVATE -march=native)
endif()
