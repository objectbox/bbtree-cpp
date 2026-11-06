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
- [ ] node config, page sizes (D59); layouts as constexpr math (D9, D13, D15, D21, D29, D34)
- [ ] `slots` (the one memory primitive, D14), pages, tagged node pointers (D52)
- [ ] inline keys: `inline_key<K>` specializations, node plan (D18, D31, D43, D47)
- [ ] vector counts: SSE2, POPCNT/AVX2, AVX-512, NEON, scalar (D40, D56)
- [ ] leaf and internal node operations and searches (D19, D36, D38, D49); node tests

## M2 Unsync tree (`detail/tree/`)
- [ ] root page, tree, shared prefix (D50, D51, D52); `get`
- [ ] node stacks (D24); insert with split (D32, D33, D37), last-leaf append (D45)
- [ ] remove with rebalancing (D22), prefix refits (D26, D44)
- [ ] walks: iteration and ranges (D23, D33, D35, D42), teardown
- [ ] invariant checks (`BBTREE_CHECKED`); differential tests against `std::map`

## M3 Map and set API (`unsync::bbtree_map`, `unsync::bbtree_set`)
- [ ] map API: lookup, insert, erase, iterators, ranges, entries
- [ ] bulk: builder (D25), from sorted / from unsorted, append, split_off, clone
- [ ] retain, extract_if
- [ ] set API and set algebra
- [ ] port `unsync/map/tests`, `unsync/set/tests`

## M4 Batches (D53)
- [ ] lanes, `get_many`, `insert_many`, set `contains_many`/`insert_many`

## M5 Shared map (`bbtree_map`, D54, D55, D57)
- [ ] latch word, optimistic reads, page pool (type-stable pages)
- [ ] lookup, insert, remove, scans, batches, append
- [ ] stress tests (Shuttle stand-in), TSan preset

## M6 File maps (D58) and bbtree-mmap
## M7 bbtree-store (D60): leaf annex, heap pages, records, WAL
## M8 Fuzz targets (libFuzzer), racing-reads checker, kernel-target build
## M9 Bench harness (std::map, abseil btree_map, ankerl, std::unordered_map)
## M10 READMEs and docs
## M11 Measure against the Rust original, profile, optimize
