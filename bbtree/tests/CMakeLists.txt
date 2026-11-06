# The library's tests, one executable, which CTest runs test case by test case.
add_executable(bbtree_tests
  allocator_test.cpp)
target_link_libraries(bbtree_tests PRIVATE bbtree::bbtree bbtree_warnings Catch2::Catch2WithMain)
catch_discover_tests(bbtree_tests DISCOVERY_MODE PRE_TEST)
