# bbtree-store: a memory-bounded store of byte strings on bbtree's shared map,
# every byte of its keys and values in pages from its allocator; and its WAL.
add_library(bbtree_store INTERFACE)
add_library(bbtree::store ALIAS bbtree_store)
target_include_directories(bbtree_store INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bbtree_store INTERFACE bbtree::bbtree)

if(BBTREE_BUILD_TESTS)
  find_package(Threads REQUIRED)
  add_executable(bbtree_store_tests tests/store_test.cpp tests/wal_test.cpp)
  target_link_libraries(bbtree_store_tests PRIVATE bbtree::store bbtree_warnings Catch2::Catch2WithMain
                        Threads::Threads)
  catch_discover_tests(bbtree_store_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "bbtree_store_tests: ")
endif()
