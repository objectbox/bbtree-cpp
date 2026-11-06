# The libFuzzer targets (Clang only, `--preset fuzz`), one per harness and row
# of the node plan, each a thin binary over a harness header.
set(BBTREE_FUZZ_TARGETS map_integers map_heap_byte_strings map_byte_arrays map_user_inline_keys
    map_plain_keys sync_map set_algebra hostile_keys)
foreach(target IN LISTS BBTREE_FUZZ_TARGETS)
  add_executable(fuzz_${target} targets/${target}.cpp)
  target_link_libraries(fuzz_${target} PRIVATE bbtree::bbtree bbtree_warnings)
  target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
endforeach()
