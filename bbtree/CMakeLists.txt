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

  # The maps' code paths without SSE2, as a kernel's build takes them.
  add_executable(bbtree_kernel_target kernel-target/kernel_target.cpp)
  target_link_libraries(bbtree_kernel_target PRIVATE bbtree bbtree_warnings)
  target_compile_options(bbtree_kernel_target PRIVATE -U__SSE2__ -U__POPCNT__ -U__SSE3__ -U__SSSE3__
                                                     -U__SSE4_1__ -U__SSE4_2__ -U__AVX__ -U__AVX2__ -Wno-builtin-macro-redefined)
  add_test(NAME bbtree_kernel_target COMMAND bbtree_kernel_target)
endif()
