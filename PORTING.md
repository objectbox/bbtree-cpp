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
  (`--preset tsan`); the GitHub workflows become one (`.github/workflows/
  ci.yml`: format, Clang and GCC tests, both sanitizers, a minute of each
  fuzz target, the bench's self-check). The Nix flake, which pins the Rust
  toolchain, is not carried over: the workflow builds with Ubuntu 24.04's
  default Clang, GCC and CMake, and pins only clang-format (P2).
- **Left out:** the racing-reads checker (P15), and the release workflows
  (crates.io publishing has no counterpart). The Verus proofs
  (`bbtree/src/proof`, the WAL's lemmas).
  There is no Verus for C++; the executable parts of their specs become
  debug assertions and tests. The Dioxus web visualizer (`bbtree/vis`, 14.5k
  lines of a Rust-to-wasm UI) is a separate product, not part of the
  library; porting it would mean a new web stack.
- **Alternatives:** port everything best effort (the visualizer alone is
  as large as the unsync map), or the library and bench only (leaves the
  store, file maps and fuzzing behind).

## P2. C++20, built and tested with Clang and GCC

- **Chosen:** C++20, no extensions, warnings as errors, Clang 15 and GCC 13,
  the versions the port was written and measured with.
- **CI builds with Ubuntu 24.04's default Clang** (18), not a pinned one,
  as the port builds with any recent Clang; the local builds keep Clang 15
  covered. It first pinned Clang 15, which stopped working when the runner
  images added GCC 14: Clang takes the newest GCC's libstdc++, and Clang 15
  can't parse GCC 14's C++20 ranges headers.
- **clang-format is pinned,** to 20.1.8 from PyPI, which `scripts/check.sh`
  runs through `uvx`: the format check needs one version everywhere (15 and
  20 lay out 50 places differently), and PyPI's build is the same small
  binary on every machine, apart from the compiler.
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
  `std::bad_alloc`, except where the API hands it back
  (`try_update_or_insert_with_leaf`, P21).
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
  fewer too. Both show in the comparison with the Rust crate (M11). A
  one-at-a-time move constructs straight into the destination slot and
  destroys the source, one move each, not two through a temporary: 12% off
  a 1M random `std::string` insert.
- **Measured and rejected:** moving libstdc++ strings as bytes and then
  pointing each short string's data pointer at its own new buffer, in every
  byte-moving path (`slots`, the paired entries' shift). It depends on
  libstdc++'s layout, and a prototype gained nothing on 1M random
  `std::string` inserts (235 ns against 229): the shifts overlap the leaf's
  cache misses. It gains on maps that stay in cache (FUTURE_WORK.md). The rest of the gap to the Rust crate's 180 ns (the Rust
  bench's `str-json` cell) is the 32-byte key: 19 entries a leaf against 24,
  so more leaves, more levels and more misses.

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

## P9. In-node counts: the compiler's, SSE2 masks, and no AVX-512 or NEON

- **Chosen:** on x86-64 the count over every inline-key slot (04, Q6) is
  written in vectors (`detail/node/vector_count.hpp`), one primitive for
  every slot width and capacity: AVX2 compares and a population count where
  the build has AVX2, SSE2 compares read as runs of mask bits otherwise
  (D40, D43), two scalar passes for 8-byte slots without SSE4.2; on x86-64
  without SSE2, a binary search. Elsewhere it is a plain loop over a
  compile-time capacity, which Clang turns into NEON compares on AArch64
  (P33). Hash-tag filtering uses SSE2 (D36).
- **It was the plain loop on x86-64 too**, left to the vectorizer, which
  holds at `-O3` only: g++ 13 at `-O2` counted a slot at a time with a
  branch each, wherever the build had POPCNT (`-march=native`, x86-64-v2 and
  up) and for string keys at any `-march`. Timed (`bbtree-bench`, `u64`):
  under g++ `-O2 -march=native` a 1k hit 59 → 11.5 ns and a 1M hit 181 → 73;
  under Clang `-O3 -march=native`, whose vectorizer did take the loop, a 1k
  hit 15.4 → 10.2 ns and a 1M hit 85 → 69, the explicit count being 39
  instructions for 55 two-byte slots where the vectorizer's was 107.
- **The count's counters have the slots' width.** With `std::size_t`
  counters Clang widened every compared 2-byte slot to 64 bits and counted
  4 slots a vector; with `uint16_t` ones, 16 (`-Rpass=loop-vectorize`). The
  same pitfall D56 describes for the Rust crate's NEON count. Measured
  (Zen 3, `u64` → `u64`, one run): 1M hit 116 → 100 ns, miss 92 → 61; 10M
  hit 321 → 289, miss 244 → 213.
- **Not ported:** the explicit AVX-512 counts and 8-byte equality masks
  (D41) and AArch64's NEON counts (D56). This machine (Zen 3) has neither
  AVX-512 nor NEON, so neither could be tested or measured here, and
  untested SIMD is a liability. They are in FUTURE_WORK.md.

## P10. Batches descend level by level, not in async lanes

- **Chosen:** `get_many` and `contains_many` move every lane down one
  level at a time: each searches its node and prefetches its chosen edge's
  slot, then each reads its edge and prefetches the child's header, and at
  the leaves each prefetches its lone candidate's entry before the full
  comparison. The only prefetches of the library are in `prefetcher`
  (`detail/tree/batch.hpp`), as D46 allows. `insert_or_assign_many` and the
  set's `insert_many` fetch every key's path with such a descent, then
  insert in input order.
- **Alternatives:** the Rust crate's async lanes (D53), as C++20
  coroutines, or a hand-written state machine per lane. D53 measured
  level-by-level lanes as its ceiling (60.8 against 74.8 ns per 10M `u64`
  hit) and kept async for its inserts' sake; coroutine frames would need a
  custom allocator to avoid the heap. Level by level is both the simpler
  code and the faster lookup.
- **Measured** (Zen 3, 10M `u64`, one run): 323 ns a hit one key a call,
  198 ns a key in batches of 64.
- Inserts in a batch search again in input order, warm from the first
  descent; the Rust crate's saved destinations, which skip the second
  search, are in FUTURE_WORK.md.

## P11. Racing reads: plain reads checked by a version, as in the Rust crate

- **Chosen:** optimistic readers (internal nodes always, leaves for
  lookups, D54, D57) read node bytes with plain loads, the header's `len`
  and `prefix_len` once each through `volatile`, and act on nothing they
  read before the node's version vouches for it. Version checks follow the
  sequence lock's order: acquire load of the version, the reads, an acquire
  fence, a relaxed reload compared with the first. `get_copied` copies a
  value's bytes into a buffer, checks the version, then `std::bit_cast`s,
  for trivially copyable values only. ThreadSanitizer is told to ignore
  exactly these reads (`racing_reads`, its dynamic annotations), nothing
  wider.
- **This is a formal data race** under the C++ memory model, as it is under
  Rust's (the Rust crate says so, `node/optimistic.rs`) and in every C++ OLC
  B-tree (Leis et al., btreeolc). The version check throws away whatever a
  torn read produced; no user code runs on it.
- **Alternative:** `std::atomic_ref` relaxed loads for every byte a reader
  races on. Not available here: mixing atomic and non-atomic accesses to
  the same bytes is itself a data race, so every writer's access to inline
  keys, edges and headers would have to be atomic too, and the node code
  moves them with `memmove` (P6).
- The latch word is reached through `std::atomic_ref` over the page's
  bytes: a page is raw memory, and `std::atomic` is not an implicit-lifetime
  type that could appear in it.
- **What a shared lookup costs over an unsync one** was open here as 21%
  at 1M `u64` hits (128.5 against 105.9 ns, a probe of `get_copied`)
  against the Rust crate's 3% (09-concurrency.md, measured on another CPU,
  at 10M). In instructions at 1M hits the two crates were alike before P48
  (unsync 407, `read` 549, `get_copied` 591; Rust 454, 555, 537), and since
  it the port's `read` is 468 and `get_copied` 492. In time, on one thread,
  unsync `find` 50 ns against 62-72 for the three shared lookups: what is
  left is the version loads' and, for `read`, the latch's two locked
  operations.

## P12. Bench dependencies include mimalloc

The Rust bench links mimalloc as its global allocator, and D15's and D8's
measurements assume it (glibc's aligned allocations cost up to twice the
memory of a 1 KiB node). Comparing the port under glibc's malloc against
the original under mimalloc would measure the allocators. So the C++ bench
links mimalloc too, a bench-only dependency beside abseil and
ankerl::unordered_dense; the library stays dependency-free.

Huge pages are part of what it measures: the Rust bench's mimalloc crate
backs its heap with huge pages (37 thousand dTLB misses in a whole 1M u64
iterate cell), and mimalloc on 4 KiB pages misses the dTLB at about every
leaf, which made the port's iteration look twice as slow (2.1 ns an entry
against 0.8 ns, the same walk). The C++ bench gets the same pages from the
same mimalloc (P17).

## P13. Exceptions from user code end the program where keys move

- **Chosen:** user code (comparators, `inline_key` specializations, key and
  value moves and destructors) must not throw, as the Rust crate's must not
  panic (D48). Key and value moves and destructors are required `noexcept`
  (`static_assert`); the tree's operations that move keys are `noexcept`, so
  a comparator or encoding that throws there ends the program by
  `std::terminate`, as the Rust crate's abort-on-unwind guards do where it
  calls user code while keys move. Constructing a value (`try_emplace`'s
  arguments, `operator[]`'s default) and allocating pages happen before
  anything changes and may throw: the map is then as it was.
- **Alternatives:** exception-neutral node code, which would reconcile every
  call into user code with the changes around it (what D48 rejects for the
  Rust crate: it constrains every structural operation, for code already
  wrong), or catching and swallowing.
- So the Rust fuzz target `user_code_panics` has no counterpart; its lying
  orders and encodings are `fuzz_hostile_keys`, checked for memory safety.

## P14. The kernel target: code paths without SSE2, not a freestanding build

- **Chosen:** `bbtree_kernel_target` builds and runs the maps with
  `__SSE2__` and `__POPCNT__` undefined, so the inline-key count takes its
  binary search and no intrinsic is compiled, as the Rust crate's
  `x86_64-unknown-none` build does.
- **Alternative:** a true freestanding build (`-ffreestanding
  -mgeneral-regs-only`). libstdc++'s headers need `long double`, which
  general registers alone rule out, and the library uses hosted headers
  (`<optional>`, `<span>`, `<string>` for byte-string keys). It would need a
  freestanding standard library and headers split by hosted-ness.

## P15. No racing-reads checker on the compiled code

- **Not ported:** the Rust crate's `racing-reads` checker, which reads
  rustc's assembly to confirm that once a node's version check passes, the
  code reads nothing more of the node before another check. Rust offers no
  fence that orders its volatile racing reads before a check, so it checks
  the compiler's output. It is not in this repository's checks.
- **What keeps a read before its check here** is not the language: the
  racing reads are plain loads, a data race (P11), so the memory model
  orders nothing, and a compiler that takes memory as unchanging may read a
  node again after the check in place of the copy it made before. Every
  check (`node_latch::is_unchanged`) is an acquire fence and an empty `asm`
  statement that clobbers memory, before a relaxed load of the version:
  Clang and GCC take either to change any memory, so what is used after it
  was loaded before it or is loaded anew because the source says so.
  Neither emits an instruction; the clobber cost a shared insert 1 to 2
  instructions.
- **Audited with the Rust crate's checker**, which reads Clang's and GCC's
  Intel-syntax assembly with small changes (the latched fallbacks of
  `get_copied` and `contains` end a path as a restart does). Clang 15, `-O2`
  and `-O3`, with and without `-march=native`: no read of a node after its
  check reaches a result in `get_copied`, `contains`, `read`, the descent,
  the scans or `insert`; what it reports is one check it can't make out, the
  absent key's in `contains` of a string, which compiles to a `sete` with no
  branch. **GCC 13 is not verified:** before the clobber and the inlined
  descent (P48) the checker passed GCC's `-O2` builds; since, GCC keeps a
  node's latch address in a register and loads the version through it,
  which the checker's model of a latch word at offset 8 of a node pointer
  does not follow, and it reports paths it can't check in every lookup.
  Read by hand where sampled (the tied separators' search), those are
  version loads. FUTURE_WORK.md has the checker for GCC and in CI.

## P16. `for_each` beside iterators, for Rust's folds

Rust's map iterators specialize `fold`: a fold runs a leaf's entries in one
loop and crosses leaves between runs, and the Rust bench's iterate checksum
folds. A C++ range-for can only call `operator++`, and the iterator keeps its
position in memory, because the outlined leaf crossings take the cursor's
node stack, which lives in the same object, by address. Every step stores
the position and reads it back. Measured over a warm 1M u64 map built in
random order, with huge pages:

| walk | Clang 15 | GCC 13 |
|---|--:|--:|
| range-for | 1.6 ns an entry | 1.3 ns |
| `for_each` on a cursor | 0.9 ns | 1.0 ns |
| `for_each`, depth-first down the edges | 0.56 ns | 0.67 ns |
| Rust, `fold` | 0.78 ns | |

- **Chosen:** `for_each(visit)` on the unsync map and set, and in the shared
  map's owner `for_each`, as a depth-first walk down the edges
  (`detail/tree/walk.hpp`) that visits each leaf's head, then its keys
  (D17): every entry, in order, with no node stack and no separator visited
  on its own. A 1k u64 map's walk takes 0.25 ns an entry against Rust's 0.34,
  and 1M maps of 4 `std::string` → 32-byte entries 10 ns against 17 on a
  cursor: the cursor's outlined crossings kept the CPU from starting the
  next map's loads early.
- **`fold(init, f)` beside `for_each`**, on the unsync map and set, as
  Rust's `iter().fold`: the walk threads the accumulator by value through
  its recursion. A visitor that adds into a variable it captures by
  reference keeps that variable on the stack, since the reference reaches
  the recursive `subtree` call: every entry stored and reloaded it, inside
  the chain of adds that waits on the entry's loads, and Clang checked per
  leaf that the entries don't overlap it. The bench's checksum folds, as
  the Rust bench's does: 1M maps of 4 `std::string` keys 7.8 → 7.2 ns an
  entry, `compact_string` keys 8.0 → 6.3 (Rust 5.1 in the same minutes).
  The walk's instructions per entry match the Rust fold's (about 22,
  sampled); what was left is in how the loads meet the prefetcher (the
  four-a-round bullet below).
- **A root leaf is walked in `run`, with no call** (`leaf_entries` is
  `always_inline`): small maps' walks are a call and a frame each
  otherwise, instructions that keep the next maps' loads out of the CPU's
  window. 1M maps of 4 `std::string` keys 8.3 → 7.8 ns an entry,
  `compact_string` keys 12.8 → 11.0 on a probe (P29).
- **A root leaf's entries four a round** (`root_leaf_entries`), each entry
  of a round read by loads of its own. In a loop of one entry a round one
  load reads every entry: over 1M maps of 4 it steps by an entry three
  times and then jumps to the next map's page, which no stride prefetcher
  follows. Four loads that each step by a page from map to map, it
  follows. Rust's fold gets this from LLVM, which vectorizes it four
  entries at a time; Clang vectorizes the C++ loop from 17 entries only. On
  a probe of 1M maps of 4 `compact_string` → 32-byte entries, 112 → 54
  cycles a map, the demand fills from memory 2.8 → 1.1 a map, and 6.1 →
  3.5 ns an entry (Rust's fold 4.6); maps of 5 and 7 entries gain as much,
  8, 12 and 16 stay where they were. So what the bullet above left in
  memory was not the lines but the order of the loads that read them.
- **Only the root leaf, and only up to 16 entries:** in `leaf_entries`, the
  rounds beside the one loop made a 1k `u64` walk 9% slower (0.239 ns an
  entry against 0.218, three runs each), though its leaves never took
  them: the loop's code changed. A root leaf is the leftmost leaf and has
  no head, so `root_leaf_entries` reads none.
- **Rejected:** a cursor walk that keeps its position in a local and runs a
  leaf's entries in one loop, crossing leaves as the iterators do, as Rust's
  fold does. It came first; the table shows why it went.
- **Rejected:** passing the node stack to the crossings by value and
  returning it, so that no address of the iterator escapes. It copies the
  stack on every crossing, and Clang still kept the iterator in memory:
  2.0 ns.
- **Rejected:** inlining the crossings (`always_inline`). The node stack's
  own outlined calls still take its address: no change.
- **Rejected for now:** iterators with no node stack, which search again
  from the root at each crossing. They would keep their place in registers
  at the cost of a search per leaf; FUTURE_WORK.md has it.

## P17. The bench's allocator: mimalloc 3.3.2 behind `operator new`

- **Chosen:** the timing binary replaces `operator new` and `delete` with
  mimalloc's (`mimalloc-new-delete.h`), the counterpart of the Rust bench's
  `#[global_allocator]`, which also covers only the language's own
  allocations, not libc's `malloc`. The version is 3.3.2, the one the Rust
  bench's libmimalloc-sys 0.1.49 builds by default (its `v2` feature is
  off), with its default options. Version 3 madvises its arenas for
  transparent huge pages, which is where the Rust bench's huge pages come
  from (P12): a 1M `u64` iterate cell now misses the dTLB about 6.6 thousand
  times in the whole process, and iterates in 0.93 ns an entry against the
  Rust bench's 0.8.
- **Rejected:** mimalloc 2.x with `mi_option_allow_large_os_pages` set at
  startup. It gets huge pages too, but by another path than the Rust
  bench's allocator, which a comparison would then measure; and the option
  must be set before the first allocation, which static initializers make
  fragile. Interposing `malloc` (`MI_OVERRIDE`) was rejected because static
  interposition on Linux holds only for the archive members the linker
  pulls in.
- **Memory mode counts mimalloc's block sizes.** The memory binary routes
  every form of `operator new` and `delete` through one counting allocator,
  the binary's one mutable global (the Rust bench's `static ALLOCATOR`). It
  adds `mi_usable_size` at allocation and subtracts it at free, where the
  Rust bench counts the bytes asked for: an unsized `operator delete`, which
  `global_allocator` and much of the std library call (Clang 15 has no sized
  deallocation by default), gives no size to subtract. Blocks of 1 KiB and
  512 bytes, bbtree's nodes, are the same either way: a 1M `u64` map holds
  26.7 bytes an entry in both benches. Other maps' small nodes round up to
  mimalloc's size classes, a few percent.

## P18. The bench's maps, under the Rust bench's names

The CSVs line up by map name, so the C++ maps take the Rust ones' names:

| name | Rust bench | C++ bench |
|---|---|---|
| `bbtree` | `BBTreeMap`, counted | `bbtree::bbtree_map`, counted |
| `bbtree-unsync` | `unsync::BBTreeMap` | `bbtree::unsync::bbtree_map` |
| `bbtree-unsync-inline` | `unsync::BBTreeMap<_, _, WithInlineKey>`, integers only | the same map as `bbtree-unsync` |
| `std-btree` | `std::collections::BTreeMap` | `std::map` |
| `absl-btree` | none | `absl::btree_map` |
| `hashbrown` | hashbrown's `HashMap`, foldhash | `ankerl::unordered_dense::map`, wyhash |
| `std-hash` | std's `HashMap`, SipHash-1-3 | `std::unordered_map`, `std::hash` |

- `bbtree-unsync-inline`: under the Rust crate's `Auto` strategy the
  integers already have the inline keys their `InlineKey` impl gives, with
  the same node plan as under `WithInlineKey` (`page.rs` tests it), and C++
  has no strategies (P5). The cell runs the default map; the name stays so
  that `--map` and the CSVs line up. Its `bulk_build` runs, where the Rust
  bench's is `n/a` (Rust's `collect` makes the `Auto` strategy only).
  **Rejected:** `plain_keys` under this name (the opposite of what it
  names), or dropping the name.
- The hash maps are each language's usual choice, not the same algorithm:
  `std::hash` of an integer is the integer, where Rust's std hashes with
  SipHash. Ordered maps are the comparison that matters; the hash maps are
  context.
- Every operation goes through the same calls as in the Rust bench:
  `insert_or_assign` for Rust's `insert`, `operator[]` for
  `entry().or_default()`, `extract` for `pop_first` where the map has none,
  the range constructor for `FromIterator`, and `for_each` for the iterate
  checksum where the map has it (P16). The shared map has no entry API, no
  `pop_first` and no bulk build in either bench.

## P19. Bench inputs, the Rust bench's key for key

A C++ cell and a Rust cell measure the same keys in the same orders:
SplitMix64 with the Rust bench's seeds, Lemire's bounded draw, Fisher-Yates
in the same direction, the string shapes' draws in the same order,
SipHash-1-3 for the strings' distinctness (as Rust's `DefaultHasher` hashes
a `str`, with its 0xFF terminator) and for the datasets' hold-out, and the
datasets' lines split as `str::lines` splits them.

- **Checked by `self-check`:** 50 sequences of seven cells (insertion,
  absent, probe, removal and upsert orders of one-map cells, the
  many-map cells' vocabularies, picks, misses and map orders, a key list's
  samples, the struct values) are digested and compared with digests that
  the Rust bench's own code gives, from a module added to a scratch copy of
  its sources (bbtree/bench/README.md says how). All 50 agree, with the
  Rust bench at the commit PLAN.md names.
- **Serial shuffles.** The Rust bench shuffles large inputs on all cores by
  their indices, with the serial result; the C++ bench shuffles serially,
  which gives the same result by definition. Copying and sorting inputs
  run on `std::thread`s instead of rayon, keeping the Rust bench's
  threshold, so that small maps' keys stay in the timing thread's heap.
- **One fix:** the Rust bench's many-map lookups call `get` on every map,
  which `bbtree` (the shared map) lends no reference from: its
  `get_many` cells fail there (a panic). The C++ bench looks up through
  `lookup_fingerprint` for that map, as the one-map lookups do in both.

## P20. The leaf annex: a concept, hooks in the node code, and a handle

- **Chosen:** `bbtree::with_leaf_annex<S, L>` (`leaf_annex.hpp`) gives a
  shared map's leaves an `L` after their header, as D60 does. `L` models
  `leaf_annex`: nothrow default construction, `split(L&) noexcept`,
  `merge(L&&) noexcept`, and `bbtree::is_trivially_relocatable`; the map
  also checks `leaf_annex_for<L, Allocator>`, `std::move(l).release(a)
  noexcept` with its allocator. The node code constructs it in
  `node_ptr::initialize`, calls it from `leaf_node::split_around` and
  `leaf_node::append`, releases it in the teardown (after the leaf's
  entries) and when `start_tree` loses its race, and relocates it with its
  page (`move_to`). Every hook is an `if constexpr` on
  `tree_types::has_leaf_annex`; a `static_assert` keeps annexes out of
  unsync maps and file maps. Probe functions without an annex (insert,
  get_copied, erase, insert_or_assign, clear, scan; `u64` and `std::string`
  maps, Clang `-O2 -DNDEBUG`) compile to the same instructions as before,
  symbol names aside.
- **Trivially relocatable:** Rust moves an annex as bytes with its leaf; the
  node code here does too (pages are copied whole, P6), so the concept asks
  for it rather than adding a move callback to every page copy.
- **`release` takes the map's allocator** (Rust: `release(self, alloc:
  &impl Allocator)`): a concept cannot quantify over allocators, so
  `leaf_annex_for<L, A>` is checked where the map knows `A`.
- **`annexed_leaf<T>`** is the handle the closures get (the map's
  `annexed_leaf_type`): `annex()`, and the leaf's keys by index
  (`key_count()`, `key(i)`, the head first) where Rust returns an
  iterator; the store reads them by index anyway.
- **Policies take the leaf:** every insert policy's `occupied` and `vacant`
  get the `leaf_node` they run in, and removals take a
  `before_removal(leaf, key, value)` hook, a no-op for `remove`, passed by
  value so that an empty one costs no argument. **Alternative:** detect the
  policies that want a leaf with `requires` and leave the others'
  signatures alone; two calling conventions for one hook are more to
  review than one unused parameter.

## P21. Refusing an insert: by throwing, not by a result type

- **Chosen at first:** `update_or_insert_with_leaf`'s closures refuse by
  throwing (`bbtree::alloc_error`, for want of a page). They run before the
  tree changes, and every latch, held node and reserved page is let go by a
  destructor (`held_leaf`, `held_nodes`, `node_pages`), so the map is
  unchanged and the exception reaches the caller, as an allocator's refusal
  of a node page already does. Both closures return the same non-void type,
  which the call returns.
- **Now beside it: `try_update_or_insert_with_leaf`,** whose closures return
  a `std::optional`, empty to refuse, and which returns empty too where the
  allocator refuses a node page. bbtree-store inserts through it and has no
  `try` block. A store at its budget is where it matters: a cache that
  evicts and retries sees about one refusal an insert, and a refusal was
  20,724 instructions by exception against 1,842 by value. Evicting and
  retrying at the budget, 32-byte values: 2,434 → 1,017 ns an insert on one
  thread, 455 → 267 on eight; 2,000-byte values 1,627 → 743 and 346 → 256.
  The map's other inserts compile to what they did (the probe functions of
  P20, Clang `-O2`: 161 functions, none differs).
- **Alternative:** Rust's `Result<R, Layout>` and `InsertError<K, V>`, which
  hand the entry back: an optional-or-entry result threaded through every
  insert path of `latched_changes`, and a second, non-throwing way for the
  split's page reservation to fail. More code on the hot path's types for
  one caller, bbtree-store, whose key and value need not come back.
- **Cost of the throwing form:** an exception per refused insert, for
  callers whose closures refuse rarely.

## P22. Reverse scans of the shared map

- **Chosen:** `scan_rev(visit)` and `scan_rev_from(end, visit)`, the mirror
  of `scan` and `scan_from` (`latched_scan::back_from`): the last leaf down
  last edges; a leaf's keys from the last, then its head; the left neighbor
  from the nearest level with an edge left of the path, down last edges; and
  a fresh path by the held leaf's first key when a check fails.
  bbtree-store's `scan_rev` and `range` need them.
- **Bounds stay with the caller:** as for forward scans, a scan starts at
  one bound and the visitor stops it at the other. The Rust crate's scans
  take `RangeBounds` and resumable cursors (`ScanResume`, `LeafStamp`),
  which no caller here needs yet.

## P23. bbtree-store: what is ported, and its API in C++

- **Ported:** the store (`store.hpp`), records and their keys
  (`record.hpp`), heap pages, chains and compaction (`heap_page.hpp`,
  `leaf_heap.hpp`), overflow pages, copied ranges (`range.hpp`), and the
  WAL (`wal/`), with the Rust crate's tests (`tests/store.rs`,
  `tests/wal`, the record tests) and two more: threads changing the same
  keys while others read and scan, under TSan; and the WAL's encoding
  properties the Verus proofs covered (P28). **Left out:** the Verus proofs
  (P1), and the store's bench example, which goes with the bench (M9).
- **Keys and values are `std::span<const std::uint8_t>`**, the Rust
  crate's `&[u8]`; `bytes_of(std::string_view)` views a string as one.
- **`insert` returns an `insert_result`** (inserted, replaced,
  key_too_long, value_too_long, refused) for `Result<bool, InsertError>`:
  a refused insert is an expected outcome under a budget, not an exception.
  Inside, the store's closures refuse by throwing `alloc_error` (P21), and
  `insert` catches it, giving the value's overflow pages back.
- **Ranges:** `key_range` of two `key_bound`s (unbounded, included,
  excluded) for `RangeBounds`; a range that starts after it ends throws
  `std::invalid_argument` where Rust panics. `scan`'s visitor returns
  false to stop, for `ControlFlow<R>`: the caller keeps what it would break
  with. `range_copies` has `next` and `next_back` returning
  `std::optional`, the Rust crate's double-ended iterator.
- **`store_key`'s pointer is a `mutable` plain pointer,** where Rust has an
  `AtomicPtr` for interior mutability through `&StoreKey`. Every read and
  write of it is ordered by a latch: the key's leaf's for every key, and
  for a head, which the store never changes (P24), the internal node its
  separator is in. Byte moves of keys (P6) stay plain `memmove`s.
- **Generations are 7 bits** (wrapping at 128 instead of 256): a record's
  generation byte keeps the stub bit beside it (P24). Strays still come
  home two generations on.

## P24. A leaf's head key's record stays put

- **The race:** a separator is the head key of the leaf right of it (D49),
  and a thread holding an internal node reads it without holding that leaf,
  to refit the node's prefix (`refit_prefix`, `refit_children_to_fences`).
  The Rust store moves records in compaction and pulls, and points keys to
  new records when values grow, holding only the key's leaf. So a split
  refitting a node read a head key's record while the head's leaf slid
  records over it: ThreadSanitizer reported it in the store's threads
  test (a refit in `refit_split_halves` against a compaction's `memmove`
  and pointer swap). The Rust crate has the same interleaving; its
  `AtomicPtr` hides the pointer's race, not the bytes'.
- **Chosen:** the store leaves the head key, and every byte a separator's
  reader reads (its key length and key bytes), as they are while it is the
  head. Compaction treats the head's record as another leaf's (its page
  isn't slid, and goes only as an orphan), pulls skip it, and a value too
  long for it goes into a new record, the target, which the head's record
  becomes a stub of, in place: the stub keeps its key and points to the
  target, which moves and changes as any record does. To become a stub in
  place, every record keeps at least a link's room (8 bytes) after its key.
  `annexed_leaf` gained `has_head` and `is_head`, and its contract says
  the head is not the closure's to change.
- **Alternatives:** (1) refits latch each head leaf whose key they read:
  they run mid-change, where no operation may restart, and a waiting
  refitter could deadlock with another unless waits only go down the tree,
  which would hold but needs every key read in the node code to know which
  leaves its thread already holds. (2) Keep a replaced head's old record
  alive (a zombie) until no reader can hold its address: no point is known
  where that is true without epochs. (3) Skip refits that grow prefixes in
  annexed maps: weaker inline keys for every store search, and an invariant
  relaxed.
- **Cost:** a record of a value under 8 bytes takes up to 8 more bytes
  (17 + key bytes for an empty value, against 9 + key); a head whose value
  outgrows its record takes a stub and a target until its key is written
  again when not a head, or removed. Records of values of 8 bytes or more
  are as in the Rust crate. **What the store bench shows is neither:** with
  heads free to move (one thread) the port takes the Rust crate's 103.6
  bytes an entry and 1.52 bytes of heap a live byte exactly, so the 110.6
  and 1.74 it takes are holes in pages that a head's record pins, which
  compaction can't slide; pinning also makes 45-59% more compactions.
- **The race is the Rust crate's:** ThreadSanitizer reports it on its
  unmodified sources (`3ab503d`) with one test file added, 24 to 44 reports
  a run: `tree/prefix.rs:131` and `node/internal.rs:496` read a head's key
  while `leaf_heap.rs:458` moves its record. Its CI runs the map's tests
  under ThreadSanitizer, not the store's.

## P25. The WAL is synchronous

- **Chosen:** `wal_storage` is a concept of three blocking calls (`append`,
  `len`, `read`) and `wal_writer`'s `open` and `append` return once the
  storage answered. The Rust crate's are `async`, so that an S3 backend's
  milliseconds don't hold a thread.
- **Alternatives:** C++20 coroutines (`co_await` on each storage call),
  which need an executor the standard doesn't have and every caller to
  pick one; or callbacks, which turn the writer's retry and read-back
  logic into a state machine. The writer is one thread appending one
  append at a time, which blocks just as well; a backend can still run
  its I/O on its own threads.

## P26. WAL errors: exceptions, and the storage's `std::error_code`

- **Chosen:** the writer and `read_value` throw `wal::wal_error`
  (`kind()`: storage, tail_moved, corrupted, with `code()`, `file()`,
  `offset()`), the Rust crate's `WalError`. A storage's `append` returns an
  `append_result` (appended, offset_mismatch, or outcome_unknown with a
  `std::error_code`), as the writer retries on it; `len` and `read` throw
  `std::system_error`, which the writer turns into a storage `wal_error`.
- **Why exceptions:** every WAL error ends the operation, and opening
  again is the recovery, as for I/O errors elsewhere in C++ (std::
  filesystem). C++20 has no `std::expected`.
- **One difference:** a `read_value` past its file's end is `corrupted`
  (the address doesn't hold its record), where Rust reports the storage's
  `UnexpectedEof`.

## P27. CRC32C: a table, a byte at a time

- **Chosen:** the reflected Castagnoli polynomial (0x82F63B78), all ones in
  and out, the `crc32c` crate's and S3's, from a 256-entry `constexpr`
  table; checked against the standard check value and RFC 3720's.
- **Alternatives:** SSE4.2's `crc32` instruction, as the `crc32c` crate
  uses, or slicing by 8: faster for long records, but a second path to
  test per CPU. The WAL appends once a commit (every 1 ms); a CRC of a few
  KB is microseconds either way. In FUTURE_WORK.md.

## P28. The WAL's proofs become tests and assertions

- The Verus specs of `record.rs` and `tail.rs` (P1) become tests: every
  record decodes from its bytes whatever follows them, a record cut short
  decodes to nothing, any flipped bit fails its check, and a scan reads
  back what appends wrote, with the same addresses and tail, picking up
  where it stopped. Their preconditions (lengths fit in `u32`, appends not
  empty) are assertions, or `std::length_error` for the caller's records.

## P29. `compact_string`: 12 bytes in place, the size in one place

The C++ addition the M11 comparison asked for: libstdc++'s `std::string`
is 32 bytes and points into itself while short, so `std::string` keys fill
a leaf with fewer entries than the Rust crate's `String` and shift one
object at a time (P6). `bbtree::compact_string` (`compact_string.hpp`) is
an owning byte string for keys that does neither.

- **Chosen:** 16 bytes, the size in bytes 12..16 in either form; up to 12
  bytes in place, longer strings on the heap behind a pointer in bytes
  0..8. No pointer into itself, so it is `is_trivially_relocatable` and
  shifts by `memmove`; `byte_string` gives it std::string's inline keys. A
  `std::string` → `u64` leaf entry shrinks from 49 bytes to 33.
- **Measured and replaced: 15 bytes in place**, byte 15 holding `15 - size`
  for a short string and a tag for a long one, whose size took bytes 8..15.
  It held more JSON object keys in place (91.7% of the bench's `str-json`
  against 83.9%), but `size()` became eight instructions, a shift, a mask
  and a conditional move between the forms' sizes. The walk over 1M maps of
  4 entries waits on memory, about 2.5 maps' loads in flight, and how many
  overlap depends on how many maps' instructions the CPU's reorder window
  holds: the decode alone took it from 8.2 ns an entry (reading the same
  word undecoded) to 12.8. With the size in one place, 11.2 → 8.0 ns in the
  bench, and 100k maps of 32 8.6 → 5.4. Writes and lookups stayed within
  10% (CPP_MAPS.md); misses in 1M maps went 165 → 176 ns, as more keys'
  ties read past the node.
- **A template, `basic_compact_string<Size, ShortCapacity>`,** of a 2- or
  4-byte size and 16 or 24 bytes in all, named in its two main forms:
  `compact_string` (4-byte size, 12 in place) and `compact_string_24`
  (4-byte size, 20 in place). Measured on every string cell, one run each
  (CPP_MAPS.md): a 2-byte size with 14 in place ties `compact_string` but
  for misses in 1M JSON-key maps (164 ns against 181, fewer keys' ties read
  past the node), and caps strings at 65,535 bytes, so it is not the
  default. 24 bytes with 20 in place holds most `str-16` keys in place:
  hits 12% faster (32.9 ns against 37.5 at 1k; 219 against 243 at 1M), at
  8-11% on inserts and 7-20% on walks, its entries being larger.
- **Alternative: Umbra's "German" string**, 16 bytes too: a 4-byte size, a
  4-byte prefix, and 8 more bytes in place or a pointer, so 12 bytes in
  place, as here. Its prefix lets a database compare strings without
  loading their bytes. bbtree's nodes already keep an 8-byte inline key of
  every key (D18) and its length (D36), and a search reads a stored key's
  bytes only past them (`search_key::cmp_tied` starts at the window's
  prefix length plus its width; `equals_whole_bytes` runs after the tag and
  the inline key matched). Only the sorted-append check compares from byte
  0. The prefix would save no load, so a long string leaves bytes 8..12
  unused, and a short one's bytes start at byte 0, where a long one's
  pointer is.
- **Alternative: libstdc++ strings moved as bytes** with a pointer fix-up:
  undefined behaviour and libstdc++'s layout (P6, FUTURE_WORK.md).
- **`data()` is a branch** on the size: Clang does not load the pointer
  ahead of the test, however the source selects it (checked in its
  assembly). It is not zero-terminated for a 12-byte string, so there is no
  `c_str()`.
- **API:** it converts implicitly from anything a `std::string_view` is made
  from and to `std::string_view`, explicitly to `std::string`; it compares
  with those types without a conversion. With `std::less<>`, lookups by
  `std::string`, `std::string_view` and C strings make no `compact_string`
  and compare by bytes, as `std::string` keys do.
- **Not transparent for `std::string`:** a `bbtree_map<std::string, V>`
  can't store compact strings, as iterators hand out `const K&`. Inputs take
  `std::string` as they are; outputs are `const compact_string&`, which
  converts to `std::string_view` and explicitly to `std::string`, but code
  that binds `const std::string&` to a key needs a copy. A map of
  `std::string` keys that stores compact strings would need a proxy key in
  `entry_ref` (FUTURE_WORK.md).
- **Scope:** keys of the unsync map and set. Values of the shared map are
  untested: its unlatched copies of byte-string values (D57) assume a
  layout of their own.

## P30. Loops that store into a page keep their state in locals

- **Chosen:** a loop that writes a node's page keeps what it reads every
  round in locals, not in members: the bulk build's run of a leaf
  (`tree_builder::extend`) holds the border leaf, its length, a copy of
  what every key shares and the count of entries it added. Rust's `extend`
  has the same shape, and there `&mut self` tells the compiler that nothing
  else writes the builder. In C++ a store into the page, through
  `std::byte*`, may write any member, so the members were read back after
  every store and the count stored every round: 24.5 cycles and 92
  instructions an entry of a 1M `u64` build against Rust's 19.1 and 80. In
  locals, 16.4 and 72, and the build takes 5.4 ns an entry against Rust's
  5.7.
  Splitting `push` into an inline run and an out-of-line rest alone gained
  nothing: the call was not the cost.
- **What every key shares is read whole:** `shared_prefix::word_at` loads 8
  bytes of its array and masks those past its length, where a window of
  `bytes()` shorter than 8 bytes copied them through a `memcpy` call into
  the stack and loaded them back as one word, which stalls the load. Every
  entry of a bulk build checks it, so keys sharing 1 to 7 bytes, as dense
  ids do, built at 13.7 ns an entry (52 cycles) against Rust's 7.1 (26);
  now at 6.0 (20).
- **An append checks the prefix out of line:** inlined into the insert
  path, the windows' loop took registers from it, and an append past the
  last key spilled the leaf's length and loaded it straight back: 1k
  sequential `u64` inserts 6.9 → 8.2 ns, 1M 10.1 → 11.4, though no key of
  those cells shares a byte, so the loop never ran (found by bisecting the
  bench docs' remeasurement). `is_shared_by_once` answers an empty
  prefix inline and calls for the windows: 7.3 and 10.2 ns.
  Rejected: comparing bytes again (the `memcpy` stall back for dense ids);
  the whole check out of line for every caller (a bulk build calls it
  every entry: dense ids 6.0 → 7.6 ns an entry). The shared map's append
  keeps the inline check: with `is_shared_by_once` there, 1k sequential
  `u64` inserts went 8.8 → 10.7-11.1 ns and 1M 12.0-12.4 → 14.3-14.5 (two
  interleaved runs each), at 1M 42 instructions an insert more; 16-byte
  strings at 1k were unchanged. A profile never reached the out-of-line
  windows, the prefix being empty, so the instructions are in how the
  append compiled around the call; not looked into further.
- **Measured** on one-shot probes of each language's build in a fresh
  process, as the bench cell runs it (one build, cold pages), counted as
  the difference to the same process without the build, built with
  `NDEBUG` as the bench preset is. The earlier probe
  rebuilt the map 30 times on warm pages and matched the Rust bench's cell,
  which hid the gap (COMPARISON.md).

## P31. A store scan's visit inlines into the map's loop

- **Chosen:** `bbtree_store::scan` and `scan_rev` hand the map a visit that
  is `always_inline`, and `record::value()` resolves a stub without
  recursing (a stub's target is never a stub), so that it inlines too. Each
  entry's record is a cache miss in a heap page (the scan's records lie in
  insertion order, not key order, in both languages); misses whose
  addresses the leaf already holds overlap, as many as the CPU's window
  takes entries. As calls, the visit and `value()` made an entry 78
  instructions against the Rust scan's 56, and 121 cycles against 90. Now
  47 instructions and 94-97 cycles: a 1M entry scan 27.7 → 21.0 ns an
  entry on a probe, against Rust's 19.1. The rest is the port's 7 more
  heap bytes an entry (P24), 7% more pages to miss in.
- **Measured** on copies of both `examples/bench` that scan 20 times,
  counted as the difference to one scan, with `NDEBUG` as the bench preset
  builds.


## P32. `pooled_allocator`: pages packed into chunks, opt-in

- **Chosen:** `bbtree::pooled_allocator` (`bbtree/pooled_allocator.hpp`), an
  allocator beside `global_allocator`, which stays the default. Its copies
  share one pool. The pool carves the pages of each layout one after another
  from chunks that double from one page to 2 MiB; on Linux the 2 MiB chunks
  are `mmap`ed, aligned to 2 MiB and `madvise(MADV_HUGEPAGE)`d, so that they
  come zeroed and on huge pages where transparent huge pages are on for
  regions that ask (Ubuntu's default, `madvise`). Pages given back go on a
  free list per layout and are reused; chunks go back to the system when the
  last copy of the allocator goes. A mutex guards the pool, so maps on
  several threads, and the shared map's threads, may share one. Layouts that
  don't pack back to back, and pages over a quarter of a chunk, go to
  `operator new` directly. Constructing the allocator allocates the pool, which may throw
  `std::bad_alloc`; a map default-constructed on it ends the program then,
  its constructor being `noexcept`, so a caller that handles running out of
  memory constructs the allocator first. A pool made at the first page
  instead would split copies made before it.
- **Why:** the bench runs on mimalloc, as the Rust bench does (P17), so it
  never measured what a map gets by default. A third bench binary,
  `bbtree-bench-system`, times cells on the system's allocator (glibc 2.39
  here). There the global allocator's aligned `operator new` places 1 KiB
  pages with gaps between them and on 4 KiB pages, and bbtree++ lost far
  more than the other maps (1M `u64` entries, ns an operation, iterate and
  bulk build an entry):

  | cell | mimalloc | glibc | glibc, pooled |
  |---|--:|--:|--:|
  | random insert | 88.0 | 142 | 90.2 |
  | sequential insert | 11.4 | 34.7 | 14.8 |
  | get hit | 83.1 | 111 | 92.2 |
  | get miss | 59.9 | 84.0 | 66.6 |
  | iterate | 0.715 | 2.34 | 0.773 |
  | range64 | 301 | 513 | 296 |
  | bulk build | 4.93 | 24.8 | 8.48 |
  | 10M random insert | 264 | 318 | 257 |
  | 10M get miss | 166 | 216 | 184 |

  On glibc, abseil's B-tree, whose 256-byte nodes `malloc` packs, lost
  1.15-2.2×, so bbtree++'s lead over it mostly went. On mimalloc the pool
  runs the same instructions, and in four windows of runs its cells fell on
  either side of the global allocator's: JSON-key lookups 7% slower in one
  and even in the next, `u64` lookups 18% faster in a third. One binary's
  cells moved by up to 13% between two runs, and a walk by 2× (0.74 and
  1.60 ns an entry). Only bulk builds were slower in every pair, eight of
  them, by 7-17%, the extra time outside user code, where the pool's fresh
  chunks are faulted in (CPP_MAPS.md, "On glibc's allocator").
- **Why not the default:** a pool per map costs small maps. A pool is about
  200 bytes of its own, and the doubling chunks leave pages unused: 1M maps
  of 4 entries took 331 bytes an entry against 275, 100k maps of 32 took 237
  against 147, and building them took 357 ns an entry against 315 on glibc
  and 92 against 78 on mimalloc. Many maps that share one allocator pay none
  of it and gain what large maps gain: on glibc, 1M maps of 4 built in 91.6
  ns an entry against 314 and iterated in 5.67 against 7.72 (an experiment
  with a pool shared by every map of the cell; mimalloc 75.8 and 5.29). A
  default that shared one pool would be a mutable global (AGENTS.md) whose
  memory never goes back. And a pool keeps a shrunk map's pages for reuse,
  as a `std::vector` keeps its capacity, where the global allocator gives
  them back: a choice for the user to make.
- **Alternatives considered:**
  - **stx_btree's `PooledPageAllocator`**, the idea's source (another C++
    form of bbtree): chunks growing from 64 KiB to 2 MiB from `operator new`.
    A 64 KiB first chunk is 16 times a 4-entry map's root page, and
    `operator new` gives glibc's 4 KiB pages; chunks from one page and 2 MiB
    huge pages take both.
  - **Pages' layouts as template constants**, a size class per kind with no
    lookup: the list of classes holds two or three entries, a scan of a few
    compares beside a mutex and a `memset`.
  - **No mutex for the unsync map** (a policy parameter): the lock is once a
    page, about one in 50 inserts, and a pool that copies share must be safe
    for maps on different threads; not measured to matter.
  - **Returning chunks whose pages are all free:** a count per chunk and a
    search on every free, for maps that shrink a lot; left to FUTURE_WORK.md.
- **Measured** with `bbtree-bench-system` and `bbtree-bench`, `cpp` suite,
  `bbtree-unsync` against `bbtree-unsync-pooled` (opt-in), one run each at
  load under 0.3; the get hit row lies within the 78-122 ns that cell moves
  between runs (CPP_MAPS.md, "On glibc's allocator"). Memory with
  `bbtree-bench-memory`. The memory binary counts `operator new`, not the
  pool's `mmap`ed chunks, so the pooled rows of 1M-entry maps read low
  there; resident memory grew by 33.7 bytes an entry for 1M `u64` entries
  against 25.3, the last chunks' unused pages and huge-page rounding.
- **The shared map on 8 threads:** P37's probe (1M random `u64` inserts,
  then gets, updates and removals), each operation's share of the run's
  time. On glibc, which P37's figures were taken on, the pool made inserts
  faster: 49-53 ns against 74-77 for the first map in a process, and 52-53
  against 56-57 for a second, which the global allocator builds in the
  first one's freed pages. On mimalloc it made them slower, 49-54 ns against
  40-42, for a first map and for a second on the same pool. Gets, updates
  and removals came within a few ns on either allocator; on one thread, on
  mimalloc, the pool matched the global allocator. The mutex took under
  0.1% of an 8-thread profile (taken before the fix below).
- **Found** by that probe: the shared map's pages had missed their huge
  pages. Its first touch of a fresh page is a read, which on Linux (6.8
  here, `use_zero_page` on) maps the huge zero page; the write after it
  then splits that into 4 KiB pages. In the probe's `smaps` every 2 MiB
  chunk showed no huge page while mimalloc's arena was backed whole, and a
  bare chunk, mapped and advised as the pool does, got its huge page when
  written first and none when read first. The pool now writes each 2 MiB
  chunk once when it maps it. Before, the 8-thread probe took 6.5M dTLB misses
  and 22k page faults on mimalloc, now 16k and 290, and the pool lost
  inserts on both allocators (73-79 ns on glibc, 73-77 on mimalloc). The
  unsync map writes a page before reading it: its cells took the same dTLB
  misses before and after, and the figures above are unchanged by the fix.

## P33. AArch64 and macOS: built and checked, not measured

- **Chosen:** the library compiles for AArch64 and macOS with what it had
  (P9: the in-node count is a plain loop, which Clang turns into NEON
  compares, `cmhi`/`cmeq` on 8 × 16-bit and 4 × 32-bit lanes; hash tags
  take the scalar loop beside the SSE2 one), plus two fixes:
  - **The latch's spin hint.** Its backoff spun on `_mm_pause` on x86 and
    on nothing elsewhere, an empty loop the compiler may delete, so that a
    waiting thread yielded to the scheduler at once. It now issues `isb` on
    AArch64, as Rust's `core::hint::spin_loop` does there, and a compiler
    barrier on other targets.
  - **`MADV_HUGEPAGE`** is Linux's: bbtree-mmap asks for huge pages only
    where the macro exists, so it compiles on macOS. `pooled_allocator`
    (P32) maps its 2 MiB chunks only on Linux and takes them from
    `operator new` elsewhere.
- **Checked:** the library, the store, the file maps and the tests
  type-check for `aarch64-linux-gnu` with Clang 15 (`-fsyntax-only`, the
  host's libstdc++ headers and a 64-bit word-size shim, as there is no
  AArch64 sysroot here), with `-Wall -Wextra -Wconversion`; and the AArch64
  assembly shows the NEON counts and `isb`. CI gains an AArch64 Linux job
  (`ubuntu-24.04-arm`, Ubuntu's default Clang as on x86, P2) and a macOS job
  (`macos-15`, Apple silicon), which build and run the tests; both pass.
- **Native builds:** the host-CPU flag is `-march=native` on x86 and
  `-mcpu=native` on AArch64 (`BBTREE_NATIVE`, and the tests' native
  target), which Apple's Clang requires on Apple silicon.
- **libc++:** the shared map's latches use `std::atomic_ref`, which libc++
  has from version 19; the macOS job builds with Homebrew's LLVM, whose
  libc++ is newer than some Xcode versions'. With libstdc++ (GCC 13, or
  Clang on Linux) any version the port builds with has it.
- **Alternatives considered:**
  - **stx_btree's way**, GCC/Clang vector extensions over 16-byte lane
    groups for both counts and tags, one source for SSE and NEON: the count
    is already the compiler's vectors on both, and the tags' movemask has no
    NEON instruction (it takes a narrowing shift and a 64-bit mask, 4 bits a
    lane); untested on hardware here, so left to FUTURE_WORK.md with the
    NEON counts.
  - **An AArch64 cross toolchain and QEMU in CI** instead of an ARM
    runner: slower and further from real hardware than GitHub's ARM
    runners.

## P34. Installing: `find_package(bbtree)` with the targets' own names

- **Chosen:** `cmake --install` puts the headers of the three libraries
  under `include/` and a CMake package under `share/cmake/bbtree`:
  `find_package(bbtree 0.2 CONFIG)`, then `bbtree::bbtree`, `bbtree::mmap`
  and `bbtree::store`, the names `add_subdirectory` already gives, so a
  project links the same targets either way. The include paths are build-
  and install-interface generator expressions; `BBTREE_CHECKED`, on in the
  tests' builds, is the build tree's only, so an installed bbtree never has
  the checks. `BBTREE_INSTALL` (on at top level) turns the rules on. The
  version file asks for the same minor version (`SameMinorVersion`): the
  API may change between 0.x releases. `scripts/check-install.sh`, also a
  CI step, installs into a prefix, moves the prefix and builds and runs a
  consumer (`tests/installed`) against the moved copy, so that no source or
  build path is baked in. The idea of that check is stx_btree's.
- **Alternatives considered:**
  - **The package named `bbtree++`:** CMake takes the name into file names
    (`bbtree++Config.cmake`) and variables (`bbtree++_FOUND`), where `+` is
    legal but unusual and easy to mistype in tooling; the library's
    namespace and include directory are `bbtree`, and the package follows
    them.
  - **The package under `lib/cmake`:** the libraries are headers only, so
    the package is architecture-independent (`ARCH_INDEPENDENT`), and
    `share/cmake` is where CMake looks for those.
  - **A pkg-config file:** none of the port's users so far builds without
    CMake; easy to add beside the package if one does.

## P35. A short byte string's window: overlapping loads, not a copy

- **Chosen:** the window of a byte string with fewer than 8 bytes left
  (`big_endian::window`, every string key's inline key and every tie's
  comparison) is two fixed-size loads from either end of the bytes, 4 + 4
  or 2 + 2 bytes, which overlap and never read past them, shifted into
  place; one byte is one load. It was a copy of the remaining bytes into a
  zeroed buffer, which, its length varying, Clang compiled to a call to
  `memcpy` on every short window: 8 calls in a string lookup's descent.
  About half of the JSON keys are 7 bytes or shorter.
- **Measured** (`cpp`/`ci` string cells, two A/B runs, load under 0.3): 1M
  JSON keys, misses 177 → 155 and 171 → 150 ns, hits 249 → 235 and 256 →
  247, inserts 252 → 249 and 256 → 231; 16-byte keys, which have no short
  windows, unchanged.
- **Source:** stx_btree, another C++ form of bbtree, builds its short
  windows this way (`short_prefix_word`); its byte-by-byte loader is not
  merged into one load by Clang 15, so the loads here are this port's own.
- **Alternatives considered:** a fixed 8-byte load that may read past the
  bytes, masked: faster still, but a `std::string`'s bytes can end at a page
  boundary only for heap strings longer than the small buffer, and a span
  from a user's `string_view` can end anywhere, so an over-read is not safe
  in general.

## P36. A shared scan checks the levels it keeps before taking the next leaf

- **Chosen:** when a shared map's scan moves to the next (or previous)
  leaf, it walks down from the nearest node with an edge left, and then, as
  the Rust crate's `next_leaf` and design/09-concurrency.md do, checks that
  every node the path keeps below that one is still at the version read:
  they still end at the held leaf, which can't split while held, so the leaf
  found is its neighbor; if one changed, the scan searches again for the
  last key it visited. The port had checked only the node it turned at.
  No interleaving that skips entries without the check was found (the held
  leaf can't split, and rebalancing stays under one parent), but the check
  is the design's invariant and costs a load a level.
- **Found** by comparing the scan with stx_btree's, another C++ form of
  bbtree, which checks the kept levels too.
- **Untested by failure:** with the check removed (it answering "as read"
  always), the scan stress tests (`[scan]`: forward and reverse scans of
  20,000 `u64` entries, three levels, against writers that split and merge
  leaves) passed 6,000 runs in a release build. Counted, a run checks kept
  levels about 500 times (the crossings that turn at the root), and in no
  run had one of them changed by then, even with the forward scan's visitor
  slowed to widen the window: the tests don't reach the case the check
  guards. A test that does would need a hook that pauses a scan between
  leaves while a writer splits or merges a leaf beside the held one, under
  the same parent. That changes a kept level without moving the held leaf's
  neighbor, so the check would search again needlessly; whatever does move
  the neighbor across the turn rewrites a separator in the node turned at,
  whose version the scan checks already. So the check is expected never to
  be what catches an error; it stays as the design's invariant.

## P37. The shared map's scratch arrays stay unwritten until used

- **Chosen:** a change of the shared map records its path (`tree_path`),
  the nodes it holds (`held_nodes`) and the pages it reserves, takes and
  frees (`node_pages`) in arrays sized for the deepest tree, of which it
  uses a few entries. Their element types zero themselves when
  default-constructed (`latch_version`, `node_ptr`), so every insert that
  split and every removal that rebalanced first wrote about 2 KiB of zeros:
  an 825-byte `memset` and some 200 scalar stores in `insert_splitting`
  alone. `unwritten_array<T, N>` keeps the elements in a union its
  constructor leaves alone; assigning one begins its life (trivially
  copyable types only, checked). Single-threaded, the 1M-key probe (insert,
  get, update, remove) ran 3.6% fewer instructions; at 8 threads inserts
  went 57-58 → 54-56 ns, within the noise of that probe.
- **Alternatives considered:** making `latch_version` and `node_ptr`
  trivially default-constructible, as stx_btree's types are (where the
  finding came from): the same instructions saved, but every
  `node_ptr<T> p;` in the code would then start indeterminate where it now
  starts null; the arrays are the only place the zeros cost.
- **Checked under Valgrind:** the sanitizers this port runs (ASan with
  UBSan, TSan) don't flag a read of storage never written, so the shared
  map's tests (`[shared]`, release build) ran under memcheck with
  `--track-origins=yes`: no errors, all 14 tests in one run of 18 s. That
  run needs `--fair-sched=yes`: Valgrind runs one thread at a time, and
  under its default scheduler one thread ran alone for 82 minutes while the
  others slept and no test finished.

## P38. A step is inlined whole, so the position stays in registers

- **Chosen:** `cursor::next`, its crossing to the next leaf (the climb to
  the separator, the descent) and the attaching of a node stack are
  `always_inline`; the search that gives a lookup's cursor its node stack
  stays out of line and returns the stack by value, assigned into the
  cursor. No call made by a step then takes the cursor's address, so the
  compiler keeps the position (the key and value addresses, the leaf's end)
  in registers. The crossing had been outlined and took the node stack
  inside the cursor by reference: the cursor escaped, and every step loaded
  and stored its position, a chain from step to step (1.9 instructions a
  cycle on `range64` over 16-byte strings, against stx_btree's 3.0 with the
  same cache misses). Backward steps, `cursor::prev` and its crossing, are
  inlined the same way.
- **Measured** (`range64`, `bbtree-unsync`, two A/B runs each): `u64` at 1k
  123-128 → 94-98 ns; 16-byte strings at 1k 257-265 → 144-153; `u64` at 1M
  298-309 → 232-284. Range-for over a whole map (a probe): 1k `u64` entries
  1.34 → 0.70 ns an entry with Clang, 1.16 → 0.76 with GCC; 1M 4.15 → 3.26
  and 3.71 → 3.16; backward (`--it` from `end()`), 1k 1.37 → 1.00 and 1M
  5.4 → 4.3-4.9 with Clang. The other cells unchanged. A loop that steps
  carries the crossing's code: the forward probe's text grew by 5 KB with
  Clang, none with GCC.
- **Alternatives tried, not kept:**
  - **No node stack in the cursor**, each crossing searching from the root:
    16-byte strings 260 → 415 ns, `u64` at 1M 290 → 395; and the merged
    builds that move keys out while they step (`append`) lose their key.
  - **The crossing inlined but `next` not:** Clang then outlined `next`
    itself, a call a step: `u64` at 1k 122 → 170.
  - **Parking a crossing at the next leaf's head** (one walk down instead of
    three): 16-byte strings 254 → 228 ns, `u64` at 1k 121 → 131.
- **Found** by comparing with stx_btree, another C++ form of bbtree, whose
  scan loop makes no call.

## P39. Short string keys copied out unlatched, by their length

- **Chosen:** the shared map's `get_copied` reads a byte-string key's value
  without latching its leaf where the leaf settles the key, as the Rust
  crate does (D36, D57): the leaf's inline keys are 8-byte windows and it
  has hash tags, each key's length below 255; one key's window equals the
  query's, the query ends within that window, and that key's tag, its
  length, equals the query's: then the two are the same bytes, and no byte
  of the stored key is read. A different tag settles a miss. The value is
  copied without a latch, and all of it holds only if the leaf's version is
  unchanged after; otherwise, at a separator, for a longer query or with
  several candidates, the lookup takes the latched read as before. The port
  had read unlatched only keys ordered by their own bytes.
- **Measured** on a probe of 1M string keys of 2-8 bytes (inserts, hits
  and misses), alone: no change outside the timings' noise of about 10%
  single-threaded and at 8 threads (76-77 ns), 2.8% more instructions and
  cycles within the runs' spread; the lookups wait on memory, not on the
  latch. It is what `contains` (P41) and the latching of the leaf read
  (P42) build on, after which the same probe with `contains` ran 10.74 →
  9.52 G cycles.
- **Tested** against `std::map` with keys that tie in their windows and
  differ in length (`"p12"`, `"p1\0"`), misses one zero byte longer, keys
  whose lengths run across the tag's cap of 255 (which a bound mutated from
  `>=` to `>` fails), and readers racing writers that insert, remove and
  update (also under TSan).

## P40. A shared scan writes its path in place

- **Chosen:** a shared scan, moving to the neighbor leaf, truncates its
  path at the node it turns at and records the walk down into it, setting
  aside only the levels below the turn, which it checks afterwards (P36). It
  had built a second path, copied the levels above the turn into it, and
  copied the whole path back, 928 bytes, every leaf. stx_btree truncates its
  path in place too. A failed walk leaves the path half written, which the
  search again (`repath`) starts over from the root.
- **Measured** (a probe: 20 scans of a 1M `u64` map): 4.7 instructions an
  entry fewer, 3% fewer cycles; the time within the probe's noise.

## P41. `contains` settles unlatched where `get_copied` does

- **Chosen:** the shared map's `contains` takes the unlatched lookups of
  `get_copied` (keys ordered by their own bytes, compared as a copy; short
  byte strings, by their length, P39), without copying the value, which
  need not be trivially copyable for it. It had settled only misses whose
  inline keys ruled them out, and descended again with latches for
  everything else: a hit, and a miss whose 2-byte window tied with a key.
- **Measured** (a probe of 1M `u64` keys, one run each, the effect well
  outside the probe's noise of about 10%): hits 145 → 100 ns and misses
  157 → 90 single-threaded; at 8 threads 20 → 13 ns wall for both.

## P42. A lookup the leaf can't settle latches that leaf, not the root down

- **Chosen:** where `get_copied` and `contains` read a leaf unlatched and
  it doesn't settle the key (several candidates, a string longer than its
  window, a separator's head for string keys), they latch that leaf shared
  at the version their descent read it at (`node_latch::try_lock_shared_at`,
  which fails at once if the leaf changed or a writer holds it) and search
  it there; at a separator, the head must still be the key. Only a leaf
  that changed sends them down from the root again, latched. They had
  always descended again, so a string key longer than its window paid two
  descents once P39's unlatched attempt came first. The Rust crate's
  `latch_unchanged` does the same.
- **Measured** (probes of 1M string keys, inserts and three lookup passes;
  instructions and cycles of the whole run, the timings' noise at 10%):
  short keys 10.74 → 9.98 → 9.52 G cycles before P39, with P39 and P41,
  and with this; longer keys (11-13 bytes) 12.65 → 11.77 → 11.38.

## P43. What the second round costs in code and build time

- **Measured:** the second round (P38-P42: steps inlined whole, short keys
  and `contains` settled unlatched, lookups that latch their leaf) against
  the tree before it, Clang 15. The bench binary's code (`bench` preset,
  with LTO) grew 0.4% (`.text` 3.68 → 3.70 MB), and the tests' (`release`
  preset) 2.6% (3.16 → 3.24 MB). Building the tests' own sources, in a
  fresh build directory with Catch2 built beforehand and ccache off, on 32
  threads at load under 0.3, took 103 → 106 s of CPU (user and system) and
  16.5-16.7 s on the clock either way.
- **Before it,** the comparison with stx_btree (P32-P37) grew the bench's
  code 17% (3.15 → 3.68 MB), 0.47 MB of it in the bench's two new maps
  (the pooled one and stx_btree's), and the tests' build 94 → 103 s of CPU.

## P44. Nodes are aligned to a cache line, not to their size

- **Chosen:** a node's page is as large as before, a multiple of its kind's
  page size, and aligned to 64 bytes; D15 aligns it to its size, 1 KiB for a
  leaf, so that a node never crosses a 4 KiB page. Only bit 0 of a node
  pointer is a tag (D52), and no code reads more of an address. File maps
  keep their pages at multiples of the page size, so their bytes and
  fingerprint are as they were.
- **Why:** on glibc an aligned `operator new` of 1 KiB at 1 KiB takes 2 KiB
  of heap, as the chunk's header sits before the pointer: a map took twice
  its pages' memory, spread over twice the 4 KiB pages. Aligned to 64, on
  glibc 2.39 (1M `u64` entries, `bbtree-bench-system`): bulk build 24.9 →
  15.3 ns an entry, a walk 2.24 → 1.48, `range64` 465 → 332, random inserts
  138 → 116, a cell's peak RSS 85.5 → 61.8 MB; 1M maps of 4 entries 2,285 →
  1,370 MB. On mimalloc, whose 1 KiB blocks come aligned to 1 KiB anyway:
  the same instructions, page faults and heap bytes, times within their
  run-to-run range.
- **With it, glibc's own tunable does what the pool does:**
  `GLIBC_TUNABLES=glibc.malloc.hugetlb=1` (glibc 2.35 and later; the
  environment only, there is no `mallopt` for it) has `malloc` ask for huge
  pages, and the default allocator then matches `pooled_allocator` (P32) on
  the six cells measured. So a pooled default was not built: it would save
  no memory, and time only for programs on glibc that can't set the
  variable.

## P45. Switch points in the latched tree, and a scheduler of threads for tests

- **Chosen:** every atomic the shared map's threads share (the latch word,
  the root and last-leaf pointers, the free lists) is an
  `interleaved<A>` (`detail/latched/interleaving.hpp`): with
  `BBTREE_INTERLEAVING` unset, as everywhere but in one test executable, an
  alias of `A`, so production code compiles to what it did (the review's
  probes: byte-identical assembly under Clang 15). Set, each access first
  asks a scheduler, which runs the test's threads one at a time in an order
  drawn from a seed (`tests/support/interleaving_schedule`), so that an
  interleaving that fails runs again from its seed. It stands for the Rust
  crate's Shuttle tests, whose interleavings the stress tests didn't reach:
  the first test ported, a scan from a bound outside the shared prefix
  against a prefix shrink, failed, and its fix is the Rust crate's (the
  bound's side decided again under the border leaf's latch).
- **One `thread_local` pointer,** the thread's scheduler, in the test build
  only: the alternative is a context argument through every latch call of
  the library. AGENTS.md's rule against statics yields here, by the
  maintainer's word, as it costs production code nothing.
- **Not ported yet:** six of the seven interleavings the review listed, and
  merges, steals and page reuse under threads (FUTURE_WORK.md).

## P46. File maps: what a checked open refuses, and what the bytes are

- **A checked `open` refuses** what the Rust reader refuses by checks that
  are on in its release builds and were `assert`s here: a page offset whose
  end wraps, a root page outside the file map or in its header, a leaf below
  the root without a key, and a head flag that a leaf's place contradicts
  (none in the first leaf, one in every other), so that every head slot a
  reader reaches is one the check saw. A probe that mutates valid images
  (8,800 of them, eleven kinds of damage, `u64` and string keys, release
  build under ASan and UBSan) then reads, bounds and walks what opens: no
  report.
- **The fingerprint covers what the slots hold** (`file_storage::format`),
  so a file of plain values no longer opens as one of spans. Every file map
  written before has another fingerprint and is refused as a layout
  mismatch.
- **Plain bytes are the types that say so** (`is_plain_bytes`: the
  integers but `bool`, `std::byte`, floating point, empty types and arrays
  of them; a struct opts in), not every type without padding, which
  admitted pointers and `std::string_view`.
- **The writer puts no address of its own into the file:** edges and slots
  past a node's entries are zeros, so equal maps give equal files.

## P47. Iterators after the review

- A cursor keeps its tree's root node, not the tree's address, so iterators
  stay at their entries when their map is swapped or moved, as std::map's
  do; a tree keeps its root page for life (D52).
- Steps are `always_inline` at the iterators too: Clang had outlined
  `operator++` wherever a program had two loops over one map type, a call a
  step (33.8 → 12.9 instructions an entry), which P38's figures, from one
  loop a type, did not show. A loop of one bound and 64 steps runs 4% more
  instructions for it than before (1,287 → 1,343, and 1.4% more cycles).
- `rbegin()` and `rend()` return reverse iterators of the maps' own, which
  step the cursor backwards: `std::reverse_iterator` copied the 288-byte
  iterator at every dereference (a reverse walk of 1k entries 45 → 0.73 ns
  an entry).
- Iterators construct their cursor in place, lookups return their entry in
  two registers, and a query a transparent comparator would convert anyway
  (a string literal, an integer whose common type with the key is the
  key's) is converted before the search, which then takes inline keys.
- `erase` at an iterator a lookup made removes within its leaf where that
  takes no node above it, so `erase(find(key))` descends once; an iterator
  whose key a search no longer finds (a comparator that lies) is found by
  its address.

## P48. The shared map's descent is inlined into each operation

- **Chosen:** `descend`, the routing through an internal node, the
  unlatched leaf reads, and the insert's append and insert into its held
  leaf are `always_inline`; the tied separators' search returns its answer
  in two registers; what every key shares is copied only if its length, read
  first, is not zero; and the latch's waits take the latch by value, so that
  a latch that never waits stays in a register. The Rust crate inlines the
  same (`#[inline(always)]` on `descend`, `route_racing`,
  `find_optimistic`).
- **Measured** (instructions, 1M `u64`): `read` 542 → 468, `get_copied`
  554 → 492, `contains` 552 → 489; what a shared insert costs over an unsync
  one 236 → 138 (the Rust crate's 93), a removal 213 → 130 (100).
- **Cost:** the racing-reads checker no longer follows GCC's builds (P15).

