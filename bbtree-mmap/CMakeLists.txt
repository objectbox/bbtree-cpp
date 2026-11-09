# bbtree on disk: maps written to files and looked up where they lie.
add_library(bbtree_mmap INTERFACE)
add_library(bbtree::mmap ALIAS bbtree_mmap)
target_include_directories(bbtree_mmap INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(bbtree_mmap INTERFACE bbtree::bbtree)
set_target_properties(bbtree_mmap PROPERTIES EXPORT_NAME mmap)

if(BBTREE_BUILD_TESTS)
  add_executable(bbtree_mmap_tests tests/bbtree_map_file_test.cpp tests/var_bytes_test.cpp)
  target_link_libraries(bbtree_mmap_tests PRIVATE bbtree::mmap bbtree_warnings Catch2::Catch2WithMain)
  catch_discover_tests(bbtree_mmap_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "bbtree_mmap_tests: ")

  add_executable(bbtree_mmap_lookups examples/lookups.cpp)
  target_link_libraries(bbtree_mmap_lookups PRIVATE bbtree::mmap bbtree_warnings)
endif()

# The Rust crate's other examples, on mimalloc as they are: with the bench.
if(BBTREE_BUILD_BENCH)
  foreach(example first_query huge_pages var_bytes_lookups)
    add_executable(bbtree_mmap_${example} examples/${example}.cpp)
    target_link_libraries(bbtree_mmap_${example} PRIVATE bbtree::mmap bbtree_warnings mimalloc-static)
  endforeach()
endif()
