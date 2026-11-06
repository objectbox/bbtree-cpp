# The library: headers under include/bbtree, no dependencies.
add_library(bbtree INTERFACE)
add_library(bbtree::bbtree ALIAS bbtree)
target_include_directories(bbtree INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(bbtree INTERFACE cxx_std_20)
if(BBTREE_CHECKED)
  target_compile_definitions(bbtree INTERFACE BBTREE_CHECKED=1)
endif()

if(BBTREE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
