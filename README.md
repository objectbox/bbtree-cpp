# bbtree++

A C++20 port of [bbtree](https://github.com/XiangpengHao/bbtree), the B+-tree
ordered map by Xiangpeng Hao: the same nodes, layouts, inline keys, prefix
stripping, batches, optimistic lock coupling and file maps, in C++.

- `bbtree::unsync::bbtree_map` and `bbtree_set`: drop-in replacements for
  `std::map` and `std::set`, for one thread.
- `bbtree::bbtree_map`: the map threads share, through optimistic lock
  coupling, with closures instead of handles.
- File maps (`bbtree/file.hpp`) and bbtree-mmap: maps compacted into a file and
  looked up where they lie.
- Zero dependencies: headers only, C++20, Clang 15+ or GCC 13+.

[PORTING.md](PORTING.md) says where and why the port differs from the Rust
original, [FUTURE_WORK.md](FUTURE_WORK.md) what is left and what would be
faster at the cost of more code, [PLAN.md](PLAN.md) how the port went, and
[design/](design/) holds the original's design, which the port follows.

## Usage

### One thread

```cpp
#include <bbtree/unsync/map.hpp>

bbtree::unsync::bbtree_map<std::uint64_t, std::string> map;
map.insert({1, "one"});
map[2] = "two";
for (const auto& [key, value] : map) {  // const auto& or auto&&, not auto&
  std::printf("%lu %s\n", key, value.c_str());
}
map.for_each([](const auto& key, auto& value) { value += '!'; });  // a leaf at a time, faster than range-for
auto upper = map.split_off(2);  // O(log n)
std::array<std::uint64_t, 64> keys = ...;
auto values = map.get_many(keys);  // batched lookups, misses overlapped
```

### Threads

```cpp
#include <bbtree/map.hpp>

bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
{
  std::jthread a([&] { map.insert(1, 10); });
  std::jthread b([&] { map.insert(2, 20); });
}
map.get_copied(1);                                                      // std::optional{10}
map.read(2, [](const auto&, const auto& value) { return value + 1; });  // std::optional{21}
map.update(1, [](const auto&, auto& value) { value *= 2; });
map.scan([](const auto& key, const auto& value) { return true; });      // key order, true goes on
```

### Files

```cpp
#include <bbtree_mmap/bbtree_map_file.hpp>

bbtree_mmap::bbtree_map_file<std::uint64_t, std::string>::create("names.bbtree", names);
auto file = bbtree_mmap::bbtree_map_file<std::uint64_t, std::string>::mmap("names.bbtree");
file.get(42);  // std::optional<std::string_view>
```

### Keys

Keys get inline keys ([design/04-inline-key.md](design/04-inline-key.md))
when they are std byte strings (`std::string`, `std::string_view`,
`std::vector<std::uint8_t>`, `std::array<std::uint8_t, 16/20/32>`), 64- or
128-bit integers, or a type of yours with an `inline_key<K>`
specialization, and the comparator is `std::less<K>` or
`std::less<>`:

```cpp
template <>
struct bbtree::inline_key<my_key> {
  // bytes offset..offset + 8 of an encoding that orders like my_key, big-endian
  static bbtree::inline_word at(const my_key& key, std::size_t offset) noexcept;
  // optional: width, hash_tag, cmp_from, cmp_prefix (inline_key.hpp)
};
```

Specialize `bbtree::is_trivially_relocatable<T>` for a key or value type that
can move as its bytes, for the fastest shifts (PORTING.md, [moves](PORTING.md#p6-moves-relocate-only-trivially-relocatable-types-as-bytes)).

## Deviations from std::map

- Iterators dereference to `bbtree::entry_ref<K, V>`, a pair of references:
  `for (auto& [k, v] : map)` does not compile; `it->second = v` writes
  through (PORTING.md, [iterators](PORTING.md#p7-iterators-proxy-references-and-node-stacks-attached-lazily)).
- Every insert and erase invalidates every iterator, as for
  `absl::btree_map`.
- Comparators must be stateless; allocators are page allocators
  (`bbtree/allocator.hpp`), not std allocators (PORTING.md, [allocators](PORTING.md#p3-allocators-a-concept-of-page-allocators-not-stds-allocator),
  [comparators](PORTING.md#p8-stateless-comparators-and-the-maps-template-parameters)).
- Comparators, inline-key specializations, and key and value moves and
  destructors must not throw (PORTING.md, [exceptions](PORTING.md#p13-exceptions-from-user-code-end-the-program-where-keys-move)).

## Build and test

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
scripts/check.sh            # format, Clang and GCC builds, tests
scripts/check.sh --sanitize # plus AddressSanitizer + UBSan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan   # ThreadSanitizer
cmake --preset fuzz && cmake --build --preset fuzz                          # libFuzzer targets
```

## Layout

| path | what |
|---|---|
| `bbtree/include/bbtree/` | the library: maps, set, file maps, allocators, inline keys |
| `bbtree/include/bbtree/detail/node/` | nodes: layouts, plans, slots, leaves, internal nodes |
| `bbtree/include/bbtree/detail/tree/` | the tree: search, insert, remove, prefixes, cursors, builds, cuts, batches |
| `bbtree/include/bbtree/detail/latched/` | the shared map: latches, optimistic reads, held nodes, page pools, scans |
| `bbtree/tests/` | Catch2 tests, built for baseline and native targets |
| `bbtree/fuzz/` | libFuzzer targets |
| `bbtree/kernel-target/` | the code paths without SSE2 |
| `bbtree/bench/` | the benchmark harness, against std, abseil and unordered_dense |
| `bbtree-mmap/` | map files: mmap, keys and values of any length |
| `bbtree-store/` | a memory-bounded store of byte strings on the shared map |
| `design/` | the Rust original's design documents |

## License

Copyright (c) 2026 Xiangpeng Hao (bbtree, its design and `design/`) and
Markus Junginger (objectbox.io, the C++ port). Licensed under either of
Apache License, Version 2.0 or MIT license, at your option, as the original
is; [LICENSE](LICENSE) holds both texts.
