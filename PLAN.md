# Porting plan

The port goes in milestones; each is a run of commits that build with
warnings as errors under Clang and GCC and pass `ctest` (`scripts/check.sh`).
If the work stopped after a milestone, what is committed is usable.

Rust source: `../bbtree` at `3ab503d` (v0.2.0). Status marks: [x] done,
[ ] to do, [~] in progress.

## M0 Skeleton
- [x] CMake, presets (debug, gcc, asan, tsan, release, bench, fuzz), Catch2,
  `scripts/check.sh`, AGENTS.md, design docs, PORTING.md, FUTURE_WORK.md
- [x] `allocator.hpp`: page allocator concept, `global_allocator` (D8)

## M1 Node foundations (`detail/tree/node/`)
- [x] node config, page sizes (D59); layouts as constexpr math (D9, D13, D15, D21, D29, D34)
- [x] `slots` (the one memory primitive, D14), pages, tagged node pointers (D52)
- [x] inline keys: `inline_key<K>` specializations, node plan (D18, D31, D43, D47)
- [x] in-node counts: compiler-vectorized, SSE2 mask without POPCNT, binary search without SSE2 (D40);
  AVX-512 and NEON not ported (PORTING.md P9)
- [x] leaf and internal node operations and searches (D19, D36, D38, D49)

## M2 Unsync tree (`detail/tree/`)
- [x] root page, tree, shared prefix (D50, D51, D52); lookups
- [x] node stacks (D24); insert with split (D32, D33, D37), last-leaf append (D45), reserved pages (D8)
- [x] remove with rebalancing (D22), prefix refits (D26, D44); teardown
- [x] invariant checks (`BBTREE_CHECKED`); differential tests against `std::map`
- [x] walks: cursors for iteration and bounds (D23, D33, D42; PORTING.md P7)

## M3 Map and set API (`unsync::bbtree_map`, `unsync::bbtree_set`)
- [x] map API: lookup, insert, erase, iterators, bounds, try_emplace/insert_or_assign/emplace
- [ ] map-level differential test (iterators, bounds, erase(it)) against std::map
- [x] bulk: builder (D25), from sorted / from unsorted ranges, append by merged build, split_off by cut,
  clone by node copy
- [x] retain/erase_if (extract_if: future work, C++ has no lazy owning iterator idiom)
- [x] set API and set algebra (shared tree owner for map and set)
- [ ] port `unsync/map/tests`, `unsync/set/tests`

## M4 Batches (D53)
- [x] level-by-level lanes (P10), `get_many`, `insert_or_assign_many`, set `contains_many`/`insert_many`

## M5 Shared map (`bbtree_map`, D54, D55, D57)
- [x] latch word, optimistic reads (P11), page pool (type-stable pages), descent; read, get_copied,
  get_cloned, contains, update; owner extend; lookup stress test under TSan
- [x] inserts (append, fit, split through held nodes), removals (leaf-local, rebalancing through held nodes),
  shared-prefix shrinks holding both borders, start_tree; insert/insert_or_assign/update_or_insert/
  get_or_insert/remove/remove_if/erase; owner clear/for_each; change stress tests under TSan
- [x] forward scans (scan, scan_from), owner get_owned/for_each/clear
- [ ] later (FUTURE_WORK): reverse scans, range_cloned, the shared batches (read_many, get_many_copied,
  insert_many), unlatched copies of byte-string values (D57)

## M6 File maps (D58) and bbtree-mmap
- [x] file maps of plain keys and values: writer (level order, offsets from nodes), checked and unchecked
  open, lookups, bounds, iteration; layout fingerprint; corruption tests under ASan
- [x] bbtree-mmap: typed map files (mmap, read, huge pages), var-length keys and values as spans into heap
  pages after their leaves, lookups example
## M7 bbtree-store (D60): leaf annex, heap pages, records, WAL
## M8 Fuzz targets (libFuzzer), racing-reads checker, kernel-target build
## M9 Bench harness (std::map, abseil btree_map, ankerl, std::unordered_map)
## M10 READMEs and docs
## M11 Measure against the Rust original, profile, optimize
