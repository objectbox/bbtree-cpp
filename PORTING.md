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
  fewer too. Both show in the comparison with the Rust crate (M11). A
  one-at-a-time move constructs straight into the destination slot and
  destroys the source, one move each, not two through a temporary: 12% off
  a 1M random `std::string` insert. FUTURE_WORK.md has the byte moves.

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

- **Chosen:** the count over every inline-key slot (04, Q6) is a plain loop
  over a compile-time capacity, which Clang and GCC vectorize; on x86-64
  without POPCNT, the SSE2 mask path (D40, D43); on x86-64 without SSE2, a
  binary search. Hash-tag filtering uses SSE2 (D36).
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
- **Open (M11):** on one thread, shared `u64` hits cost 21% more than the
  unsync map's at 1M (128.5 against 105.9 ns) and 12% at 10M; the Rust
  crate's cost about 3% more (09-concurrency.md, Performance).

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
  the compiler's output. Here every check is an acquire fence followed by a
  relaxed load (P11), which the C++ memory model orders, and ThreadSanitizer
  runs the stress tests. Porting the checker would mean a second
  assembly-pattern parser for Clang's output, for what the fences already
  guarantee.

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

- **Chosen:** `update_or_insert_with_leaf`'s closures refuse by throwing
  (`bbtree::alloc_error`, for want of a page). They run before the tree
  changes, and every latch, held node and reserved page is let go by a
  destructor (`held_leaf`, `held_nodes`, `node_pages`), so the map is
  unchanged and the exception reaches the caller, as an allocator's refusal
  of a node page already does. Both closures return the same non-void type,
  which the call returns.
- **Alternative:** Rust's `Result<R, Layout>` and `InsertError<K, V>`, which
  hand the entry back: an optional-or-entry result threaded through every
  insert path of `latched_changes`, and a second, non-throwing way for the
  split's page reservation to fail. More code on the hot path's types for
  one caller, bbtree-store, whose key and value need not come back.
- **Cost:** an exception per refused insert, which happens only once a
  store's budget is spent.

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
  are as in the Rust crate.

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
