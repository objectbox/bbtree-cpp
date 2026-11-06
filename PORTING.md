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

## P12. Bench dependencies include mimalloc

The Rust bench links mimalloc as its global allocator, and D15's and D8's
measurements assume it (glibc's aligned allocations cost up to twice the
memory of a 1 KiB node). Comparing the port under glibc's malloc against
the original under mimalloc would measure the allocators. So the C++ bench
links mimalloc too, a bench-only dependency beside abseil and
ankerl::unordered_dense; the library stays dependency-free.
