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
- [~] map-level tests against std::map: iterators, bounds and erase(it) in focused tests
  (`map_api_test.cpp`), a random one for `compact_string` keys; no random one over iterators
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
- [x] reverse scans (scan_rev, scan_rev_from, P22); leaf annexes (D60, P20, P21): with_leaf_annex,
  update_or_insert_with_leaf, remove_with_leaf; annex tests (splits, merges, teardown, threads)
- [ ] later (FUTURE_WORK): range_cloned, the shared batches (read_many, get_many_copied,
  insert_many), unlatched copies of byte-string values (D57)

## M6 File maps (D58) and bbtree-mmap
- [x] file maps of plain keys and values: writer (level order, offsets from nodes), checked and unchecked
  open, lookups, bounds, iteration; layout fingerprint; corruption tests under ASan
- [x] bbtree-mmap: typed map files (mmap, read, huge pages), var-length keys and values as spans into heap
  pages after their leaves, lookups example
## M7 bbtree-store (D60): leaf annex, heap pages, records, WAL
- [x] leaf annex in the shared map (P20, P21), reverse scans (P22)
- [x] bbtree-store: records, heap pages, chains, compaction, strays, overflow pages, scans both ways,
  copied ranges; heads' records stay put (P23, P24); tests against std::map, budgets, churn, threads (TSan)
- [x] WAL: records, tail, writer, storage concept, CRC32C (P25 to P28); the Rust tests and the proofs'
  properties as tests
- [x] the store's bench example (with M11)
## M8 Fuzz targets (libFuzzer), racing-reads checker, kernel-target build
- [x] libFuzzer targets for every node-plan row, the shared map, set algebra and hostile keys; harnesses also
  run on fixed random inputs in the tests
- [x] kernel target: the no-SSE2 code paths built and run (P14); racing-reads checker not ported (P15)
## M9 Bench harness (std::map, abseil btree_map, ankerl, std::unordered_map)
- [x] the Rust bench ported (bbtree/bench): suites, cells, generators pinned to the Rust bench's by `self-check`,
  adapters, child process per cell, CSV, report, `compare` across languages, perf counters, datasets, memory
  binary; mimalloc 3.3.2 as the Rust bench's (P17-P19)
## M10 READMEs and docs
- [x] README (usage, deviations from std::map, build, layout), the crates' READMEs, PORTING.md, FUTURE_WORK.md,
  design/ copied
## M11 Measure against the Rust original, profile, optimize
- [x] cell-for-cell comparison with the Rust bench (bbtree/bench/COMPARISON.md); the first gaps were the bench's
  allocator (huge pages, P12)
- [x] `for_each` as a depth-first walk (P16), one move per shifted object (P6), bulk builds in place, the shared
  map's unlatched append check (a porting omission: 8-thread inserts 2.4x faster)
- [x] measured and left for later: libstdc++ strings as bytes (FUTURE_WORK.md), iterators that stay in registers
- [x] bbtree-store's bench example; CI workflow
## After the port
- [x] against C++ maps: abseil's B-tree, std::map, hash maps; the bench's `cpp` suite (bbtree/bench/CPP_MAPS.md)
- [x] `compact_string`, a 16-byte string key that moves as bytes, and its 24-byte and 2-byte-size forms (P29)
- [x] `fold` on the unsync map and set, the bench's checksum folding as the Rust bench's does (P16)
- [x] the gaps to the Rust bench closed: bulk builds (P30)
