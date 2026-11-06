# bbtree on disk: maps written to files and looked up where they lie.
add_library(bbtree_mmap INTERFACE)
add_library(bbtree::mmap ALIAS bbtree_mmap)
target_include_directories(bbtree_mmap INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bbtree_mmap INTERFACE bbtree::bbtree)

if(BBTREE_BUILD_TESTS)
  add_executable(bbtree_mmap_tests tests/bbtree_map_file_test.cpp)
  target_link_libraries(bbtree_mmap_tests PRIVATE bbtree::mmap bbtree_warnings Catch2::Catch2WithMain)
  catch_discover_tests(bbtree_mmap_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "bbtree_mmap_tests: ")

  add_executable(bbtree_mmap_lookups examples/lookups.cpp)
  target_link_libraries(bbtree_mmap_lookups PRIVATE bbtree::mmap bbtree_warnings)
endif()
