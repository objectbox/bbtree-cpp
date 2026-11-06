# Fuzz targets

libFuzzer targets for the library ([08-fuzzing-verification.md](../../design/08-fuzzing-verification.md)),
one per harness and row of the node plan, as the Rust crate's `cargo fuzz`
targets are. Each checks the tree's invariants (`BBTREE_CHECKED`) after every
operation, besides the answers.

| target | what |
|---|---|
| `fuzz_map_integers` | map operations against `std::map`, `std::uint64_t` keys |
| `fuzz_map_heap_byte_strings` | the same, `std::string` keys: hash tags, 8/4-byte inline keys, prefixes |
| `fuzz_map_byte_arrays` | the same, `std::array<std::uint8_t, 16>` keys |
| `fuzz_map_user_inline_keys` | the same, a key with its own `inline_key` specialization |
| `fuzz_map_plain_keys` | the same, `std::string` keys without inline keys |
| `fuzz_sync_map` | the shared map against `std::map`, on one thread |
| `fuzz_set_algebra` | union, intersection, differences and relations against std's algorithms |
| `fuzz_hostile_keys` | keys whose order and inline keys lie: memory safety only |

```bash
cmake --preset fuzz && cmake --build --preset fuzz
build/fuzz/bbtree/fuzz/fuzz_map_integers corpus/ -max_total_time=600
```

The harnesses also run on fixed pseudo-random inputs in the ordinary tests
(`bbtree/tests/fuzz_harness_test.cpp`). The Rust crate's `user_code_panics`
target has no counterpart: user code must not throw, and where keys move a
throw ends the program (PORTING.md, [exceptions from user code](../../PORTING.md#p13-exceptions-from-user-code-end-the-program-where-keys-move)).
