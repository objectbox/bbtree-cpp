# Porting decisions

How bbtree++ differs from the Rust original where the languages differ, and
why. Each entry names the alternatives that were seriously considered and
the reason for the choice. The Rust design's own decisions (`design/`,
D1–D60) still hold unless an entry here says otherwise.

## P1. Scope: what is ported, adapted, or left out

- **Ported:** the library (unsync map and set, shared map, batches, file
  maps), bbtree-mmap, bbtree-store, the tests, the fuzz targets, the bench,
  the READMEs and the design docs.
- **Adapted:** Miri becomes AddressSanitizer + UBSan (`--preset asan`);
  Shuttle's interleaving tests become stress tests under ThreadSanitizer
  (`--preset tsan`); the racing-reads checker reads Clang's assembly instead
  of rustc's.
- **Left out:** the Verus proofs (`bbtree/src/proof`, the WAL's lemmas).
  There is no Verus for C++; the executable parts of their specs become
  debug assertions and tests. The Dioxus web visualizer (`bbtree/vis`, 14.5k
  lines of a Rust-to-wasm UI) is a separate product, not part of the
  library; porting it would mean a new web stack.
- **Alternatives:** port everything best effort (the visualizer alone is
  as large as the unsync map), or the library and bench only (leaves the
  store, file maps and fuzzing behind).

## P2. C++20, built and tested with Clang and GCC

- **Chosen:** C++20, no extensions, warnings as errors, Clang 15 and GCC 13.
- **Alternative:** C++23 (`std::expected`, deducing `this`,
  `std::flat_map`'s proxy precedent in the library). The installed
  compilers support little of it, and nothing in the port needs it.
- Clang is the benchmark compiler: rustc and Clang share LLVM, so a
  difference against the Rust build is the source's, not the backend's.

## P3. Allocators: a concept of page allocators, not std's `Allocator`

- **Chosen:** `bbtree::page_allocator`, a concept with `allocate_zeroed` and
  `deallocate` taking a `page_layout`, and optional per-kind members
  (`allocate_internal`, `allocate_leaf`, `allocate_heap`), which
  `page_allocator_traits` defaults, as the Rust trait's default methods do.
  Failure is a null pointer; a map turns it into `bbtree::alloc_error`, a
  `std::bad_alloc`, except where the API hands it back (`try_insert`).
- **Alternatives:** std's `Allocator` named requirement (typed `T*`
  allocation, rebinding; no zeroing, no page kinds: the map would rebind to
  each page type and zero by hand), or `std::pmr::memory_resource` (a
  virtual call per page, and a pointer in the map).
- `allocator_ref<A>` stands for the Rust crate's `&A`, to share one.
- `global_allocator` is aligned `operator new` plus `memset`: C++ has no
  `alloc_zeroed`, and `calloc` doesn't align.

## P4. Tests: Catch2, one executable, internals reachable through `detail`

- **Chosen:** the library's internals live in `bbtree::detail` headers,
  which tests include directly, as the Rust crate's in-module `#[cfg(test)]`
  tests reach `pub(crate)` items. Classes don't befriend tests.
- **Alternative:** `friend struct test_access` in each class (test code
  named in production headers), or testing through the public API only
  (loses the node tests).
- `BBTREE_CHECKED` compiles the structural checks (`check_invariants`), as
  the Rust crate's `cfg(test)`/`cfg(fuzzing)` do; tests and fuzz targets set
  it.

## P5. Inline keys by specialization, not by strategy types

- **Chosen:** a key has inline keys if `bbtree::byte_string<K>` (std
  byte strings, built in) or `bbtree::inline_key<K>` (64- and 128-bit
  integers built in; any user type by specializing it) is specialized,
  and the map's comparator is the natural order (`std::less<K>` or
  `std::less<>`). Optional members of a specialization (`width`,
  `hash_tag`, `cmp_from`, `cmp_prefix`) are detected with `requires`. The
  node plan (D47) is a `constexpr` struct computed from them, and every
  choice it drives is an `if constexpr`.
- **Alternatives:** mirror the Rust crate's `Auto` / `WithInlineKey`
  strategies and its type-identity recognition (`type_identity.rs`), or a
  traits class with mandatory members. The Rust design exists because Rust
  has no specialization (D18's TODO says it would switch to it); C++ has
  it, so 365 lines of type identity and a strategy parameter disappear.
- `plain_keys` turns inline keys off, for comparisons and tests;
  `configured<S, C>` gives a node config (D59).
- A comparator other than the natural order gets no inline keys: the
  encoding orders like `operator<`. A reversed encoding for
  `std::greater` is future work.

## P6. Moves relocate only trivially relocatable types as bytes

- **Chosen:** keys and values move between slots by `memmove` when
  `bbtree::is_trivially_relocatable<T>` holds (trivially copyable types,
  `std::unique_ptr`, `std::shared_ptr`, `std::vector`, and libc++'s
  `std::string`; users specialize it for their own types), and one object
  at a time by move construction and destruction otherwise. A leaf of
  paired entries shifts whole entries in one copy only when both key and
  value move as bytes (D29). Keys and values must be nothrow-movable and
  nothrow-destructible (`static_assert`), as every Rust move is.
- **Alternatives:** require trivially relocatable keys and values (rules
  out libstdc++'s `std::string`, the most common key), or always move one
  object at a time (gives up the single `memmove` of every insert and
  removal for integers).
- **Cost:** libstdc++'s `std::string` points into itself for short strings,
  so `std::string` keys and values shift one object at a time. It is also
  32 bytes against Rust's 24-byte `String`: a `std::string` → `u64` leaf
  holds 19 entries against the Rust crate's 24, and a `std::string` set
  fewer too. Both show in the comparison with the Rust crate (M11).

## P7. Iterators: proxy references, and node stacks attached lazily

- **References:** a separator's key lives apart from its value (D17,
  D49), so there is no `std::pair<const K, V>` to refer to.
  `operator*` returns `bbtree::entry_ref<K, V>`, a pair of references, as
  C++23's `std::flat_map` does; `std::basic_common_reference`
  specializations make the iterators model `std::bidirectional_iterator`
  in C++20. Consequence for users: `for (auto& [k, v] : map)` does not
  compile (`const auto&`, `auto&&` and `auto` do); `it->second = v`
  writes through.
- **Alternatives for references:** keep every key in its leaf and copy
  separators into internal nodes (needs `K` copyable and breaks D17's
  one-copy-per-key), or store `std::pair` in leaves and point internal
  nodes at leaf keys (rejected by D17 itself: moves invalidate the
  pointers).
- **Positions:** an iterator holds its entry's key and value addresses;
  equality is one pointer compare, since every entry has its own key slot.
  Steps within a leaf add strides (D33, D42). The node stack (D24), which
  crossing a leaf boundary needs, is attached where a descent made the
  iterator (`begin`, `lower_bound`), and on the first crossing, by one
  search of the iterator's key, where a lookup or an insert did, which keep
  no ancestors (D32, D37).
- **Alternatives for positions:** build the node stack on every `find`
  and `insert` (an insert would pay a second descent for the iterator
  `std::map::insert` returns, which most callers ignore), or a `(leaf,
  index)` with parent pointers (rejected by D11).
- **Invalidation:** every insert and erase invalidates every iterator, as
  for `absl::btree_map`.

## P8. Stateless comparators, and the map's template parameters

- **Chosen:** `bbtree_map<K, V, Compare = std::less<K>, Allocator =
  global_allocator, Strategy = auto_inline_keys>`, with `Compare` empty
  and default-constructible (`static_assert`). Lookups with other key
  types need `Compare::is_transparent`, as std::map's do.
- **Why stateless:** nodes call the comparator deep inside searches and
  moves; a stateless one is made where needed, so no node operation
  threads a comparator through. Stateful comparators are rare and would
  cost a pointer per call chain.
- **Order:** std::map's first four positions, so that `std::map<K, V,
  C>` becomes `bbtree_map<K, V, C>`. The Rust crate puts its strategy
  before the allocator, for `BBTreeMap<K, V, WithInlineKey>`; in C++
  inline keys need no strategy (P5), so it goes last.

## P12. Bench dependencies include mimalloc

The Rust bench links mimalloc as its global allocator, and D15's and D8's
measurements assume it (glibc's aligned allocations cost up to twice the
memory of a 1 KiB node). Comparing the port under glibc's malloc against
the original under mimalloc would measure the allocators. So the C++ bench
links mimalloc too, a bench-only dependency beside abseil and
ankerl::unordered_dense; the library stays dependency-free.
