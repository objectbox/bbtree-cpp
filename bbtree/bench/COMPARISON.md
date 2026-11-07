# bbtree++ vs. bbtree (Rust) vs. standard maps

This C++ port of bbtree (bbtree++) was measured against the Rust original and standard maps.
Both ran the same benchmark cells with the same inputs on one machine; see Setup for details.

## Summary

- **Integer keys:** bbtree++ is typically 1.0-1.3× as fast, up to 1.6×.
- **String keys:** with compact strings bbtree++ is typically 1.05-1.3× as
  fast, up to 1.8×, and slower only building many 4-entry maps.
- **The shared map:** faster on one thread, about 5% slower with 8 threads.
- **bbtree-store:** inserts and gets are even or faster; scans are 13% slower.

## Integer keys

The unsync map with `u64` keys and values, ns an operation (iterate: an
entry). The cells are the bench's `ci` suite, its core set at 1k, 1M and
10M entries, named for running in continuous integration. Bold marks
the fastest map of each row. The two right columns compare different
maps: Rust's `BTreeMap` is a B-tree, `std::map` a red-black tree.

| cell | bbtree++ | bbtree (Rust) | Rust `BTreeMap` | C++ `std::map` |
|---|--:|--:|--:|--:|
| u64_u64_random_insert_1k | 29.5 | 36.0 | **17.5** | 22.7 |
| u64_u64_sequential_insert_1k | **7.07** | 8.04 | 23.4 | 28.5 |
| u64_u64_get_hit_1k | **16.0** | 24.8 | 29.5 | 23.6 |
| u64_u64_get_miss_1k | **14.6** | 23.2 | 29.3 | 22.5 |
| u64_u64_iterate_1k | **0.267** | 0.318 | 1.04 | 3.06 |
| u64_u64_range64_1k | 113 | 119 | **106** | 253 |
| u64_u64_random_remove_1k | **38.5** | 44.5 | 47.0 | 50.6 |
| u64_u64_entry_upsert_1k | **29.1** | 46.1 | 37.5 | 37.8 |
| u64_u64_pop_first_1k | 14.6 | 14.8 | 17.2 | **8.87** |
| u64_u64_bulk_build_1k | 4.96 | 3.92 | **2.39** | 31.1 |
| u64_u64_random_insert_1M | **82.9** | 92.2 | 102 | 224 |
| u64_u64_sequential_insert_1M | 10.3 | **10.2** | 46.3 | 79.5 |
| u64_u64_get_hit_1M | **93.4** | 94.0 | 151 | 230 |
| u64_u64_get_miss_1M | **62.4** | 83.3 | 147 | 198 |
| u64_u64_iterate_1M | **0.632** | 0.729 | 3.26 | 58.8 |
| u64_u64_range64_1M | 288 | **272** | 375 | 3205 |
| u64_u64_random_remove_1M | **105** | **105** | 118 | 271 |
| u64_u64_entry_upsert_1M | **84.3** | 94.7 | 96.1 | 196 |
| u64_u64_pop_first_1M | **16.7** | 18.5 | 19.8 | 40.1 |
| u64_u64_bulk_build_1M | 7.28 | 5.44 | **4.56** | 89.4 |
| u64_u64_random_insert_10M | 260 | **243** | 263 | 725 |
| u64_u64_get_hit_10M † | **246** | 255 | 301 | 484 |
| u64_u64_get_miss_10M | **188** | 197 | 344 | 480 |

† Rust's runs of this cell ranged from 231 to 255 ns this session.

The bulk build and iterate rows predate later changes: "What the first
optimization round changed" has their current figures.

## String keys

The unsync map, ns an operation (iterate, many maps: an entry). bbtree++
runs each string cell with the compact string that is fastest on it,
named in the row: `compact_string` (16 bytes, a 4-byte size, up to 12
bytes in place, the default), `compact_string<2>` (16 bytes, a 2-byte
size, 14 in place: short for `basic_compact_string<std::uint16_t, 14>`,
which has no alias) or `compact_string_24` (24 bytes, 20 in place);
CPP_MAPS.md, [compact strings' variants](CPP_MAPS.md#compact-strings-variants), has every variant on every cell.
The other maps have their language's standard string: libstdc++'s 32-byte
`std::string` (bbtree++ too, in the `std::string` column) and the Rust
crate's 24-byte `String`. `bbtree-bench cpp` and the Rust bench's
`ci tiny`, the string cells, one run each in one window, started at load
0.3. Variants within 3% of each other are within run-to-run noise. Bold
marks the fastest map of each row.

| cell | bbtree++ | compact string | bbtree++ `std::string` | bbtree (Rust) | Rust `BTreeMap` | C++ `std::map` |
|---|--:|---|--:|--:|--:|--:|
| str-16_u64_random_insert_1k | **39.2** | `compact_string` | 61.2 | 49.0 | 81.3 | 106 |
| str-16_u64_get_hit_1k | **34.5** | `compact_string_24` | 37.5 | 44.1 | 70.6 | 84.8 |
| str-16_u64_get_miss_1k | 24.7 | `compact_string_24` | **22.5** | 29.8 | 78.1 | 81.7 |
| str-json_u64_random_insert_1M | **168** | `compact_string<2>` | 254 | 196 | 314 | 530 |
| str-json_u64_get_hit_1M | **238** | `compact_string_24` | 269 | 289 | 535 | 758 |
| str-json_u64_get_miss_1M | **178** | `compact_string<2>` | 184 | 225 | 582 | 713 |
| str-json_struct32_build_many_1Mx4 ‡ | 68.3 | `compact_string_24` | 83.5 | 59.7 | **39.3** | 51.3 |
| str-json_struct32_get_many_1Mx4 | 78.8 | `compact_string<2>` | **78.3** | 83.7 | 97.9 | 99.0 |
| str-json_struct32_iterate_1Mx4 | **3.21** | `compact_string` | 5.93 | 5.04 | 6.24 | 5.12 |
| str-json_struct32_build_many_100kx32 | 60.7 | `compact_string` | 100 | 60.6 | **57.0** | 90.5 |
| str-json_struct32_get_many_100kx32 | 64.9 | `compact_string` | 72.5 | 68.3 | **63.6** | 86.8 |
| str-json_struct32_iterate_100kx32 | **5.32** | `compact_string<2>` | 7.46 | 9.49 | 10.6 | 26.1 |

‡ The Rust columns from a rerun of the cell with no compaction stall in
the kernel (`compact_stall` in `/proc/vmstat`); in the window's run its
fresh pages stalled it, 118 ns.

The `1Mx4` and `100kx32` rows are the `tiny` suite's: a million maps of 4
entries and 100k maps of 32, keys and 32-byte values.

With compact keys bbtree++ inserts, looks up and iterates strings faster
than the Rust crate, builds 32-entry maps as fast, and trails only in
building many 4-entry maps, where each map takes a full 1 KiB root page
(FUTURE_WORK.md). With `std::string` keys it looks up faster but in
32-entry maps, and inserts and builds slower (see "Where bbtree++
trails"). Iterating maps of 4
trailed until the root leaf's entries went four a round (PORTING.md,
[iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)): one load per entry, stepping by a page from map to map, which
the prefetcher follows.

## The shared map, u64 → u64

The benches run the shared map at 1k only; the 8-thread rows come from the
same probe in both languages (1M scrambled keys, 8 threads, wall time an
operation; `bbtree::bbtree_map` against `bbtree::BBTreeMap`).

Bold marks the faster of each pair: shared against shared, unsync
against unsync.

| u64 → u64, 1k | bbtree++ shared | bbtree (Rust) shared | bbtree++ unsync | bbtree (Rust) unsync |
|---|--:|--:|--:|--:|
| random_insert | **36.9** | 37.8 | **29.3** | 33.9 |
| sequential_insert | **9.51** | 11.6 | **6.86** | 7.89 |
| get_hit | **19.8** | 25.5 | **15.3** | 23.8 |
| get_miss | **17.5** | 23.7 | **14.4** | 22.4 |
| iterate (a scan) | **0.828** | 0.829 | **0.246** | 0.325 |
| range64 | **87.8** | 99.4 | **110** | 119 |
| random_remove | **46.5** | 49.3 | **38.0** | 43.2 |

| 1M, 8 threads | bbtree++ | bbtree (Rust) |
|---|--:|--:|
| insert | 45 | **42** |
| get_copied | 56 | **54** |
| update | 22 | **21** |
| remove | 42 | **40** |

## bbtree-store, `examples/bench` at 1M entries

Bold marks the smaller figure of each row.

| | bbtree++ | bbtree (Rust) |
|---|--:|--:|
| 1 thread: insert, ns | **253** | 254 |
| 1 thread: get, ns | **224** | 233 |
| 1 thread: scan, ns an entry | 28.5 (23.0 with the scan's visit inlined) | **19.1** |
| nodes + heap, bytes an entry | 110.6 | **103.6** |
| churn, heap bytes a live record byte (round 4) | 1.74 | **1.52** |
| 8 threads: insert, ns wall | **67** | 75 |
| 8 threads: get, ns wall | **36** | 39 |
| 4,000-byte values: insert / get, ns | 1879 / 721 | **1744 / 569** |

The store keeps a head's record in place (the race TSan found; PORTING.md, [heads' records](../../PORTING.md#p24-a-leafs-head-keys-record-stays-put)), which
costs up to 8 more bytes a record and the stubs churn leaves: the bytes
and churn rows.

## Setup

- **Machine:** AMD Ryzen 9 5950X (Zen 3, AVX2, no AVX-512), Linux 6.8,
  quiet (load under 0.3 at the start of each run).
- **Rust:** the original at `3ab503d`, rustc 1.99.0, its bench as shipped
  (mimalloc crate 0.1, which builds mimalloc 3).
- **C++:** Clang 15, `--preset bench` (`-O3 -march=native`, LTO), mimalloc
  3.3.2 (PORTING.md, [the bench's allocator](../../PORTING.md#p17-the-benchs-allocator-mimalloc-332-behind-operator-new)).
- **Inputs:** the same keys, values, orders and probes: `bbtree-bench
  self-check` pins the C++ bench's generators to the Rust bench's (PORTING.md,
  [bench inputs](../../PORTING.md#p19-bench-inputs-the-rust-benchs-key-for-key)).
- **Runs:** one per cell, each in a fresh process (AGENTS.md: no repeats to
  average noise away). Cells whose Rust runs varied by more than 10% across
  this session are marked.
- **Reproduce:** `bbtree-bench ci --out A` in each bench, then
  `bbtree-bench compare --head A/cpp.csv --base A/rust.csv`.

## What the first optimization round changed

| change | effect |
|---|---|
| huge pages in the bench, as the Rust bench's mimalloc has (PORTING.md, [bench dependencies](../../PORTING.md#p12-bench-dependencies-include-mimalloc), [the allocator](../../PORTING.md#p17-the-benchs-allocator-mimalloc-332-behind-operator-new)) | iteration 2× faster: most of the first measurement's gap |
| `for_each`, a depth-first walk with no cursor ([iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)) | 1M u64 walk 0.94 → 0.56 ns an entry, 1k 0.69 → 0.25, 1M maps of 4: 18 → 8 |
| one move per shifted object, not two through a temporary ([moves](../../PORTING.md#p6-moves-relocate-only-trivially-relocatable-types-as-bytes)) | 1M `std::string` inserts 252 → 221 ns |
| bulk builds: sorted ranges build in place; a leaf's run appends without the prefix shrink | 1M u64 bulk build 8.1 → 7.0 ns in the bench, 13.2 → 9.6 on a probe |
| shared map appends check the last leaf before latching it, as the Rust crate does (a porting omission) | 8-thread inserts 110 → 45 ns; the store's 143 → 67 |

The cells above predate later changes: a root leaf walked with no call,
four entries a round, and a checksum that folds, as the Rust bench's does
(PORTING.md, [iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)); the bulk build's run of a leaf in locals
([loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)); the store scan's visit inlined
([store scans](../../PORTING.md#p31-a-store-scans-visit-inlines-into-the-maps-loop)). In one window with the Rust bench, one run each:

| cell | bbtree++ | bbtree (Rust) |
|---|--:|--:|
| u64_u64_iterate_1k | **0.217** | 0.330 |
| u64_u64_iterate_1M | **0.655** | 0.909 |
| u64_u64_bulk_build_1k | **3.09** | 3.90 |
| u64_u64_bulk_build_1M | **5.41** | 5.51 |
| str-json_struct32_iterate_1Mx4, `std::string` keys † | 6.83 | **5.67** |
| str-json_struct32_iterate_100kx32, `std::string` keys | **9.32** | 11.7 |
| store scan, 1M, ns an entry | 23.0 | **20.3** |

† Across this session's windows 4.6-6.8 ns for bbtree++ and 4.9-5.7 for
the Rust bench; a rerun right after, with no compaction stall in the
kernel, took 5.62 against 5.29. Before the four-a-round root leaves the
bbtree++'s took 7.2-9.0.

## Where bbtree++ trails, and why

- **`std::string` inserts (1.25-1.3×), small string maps' builds
  (1.4-1.65×):** libstdc++'s `std::string` is 32 bytes against `String`'s
  24 (19 entries a `std::string` → `u64` leaf against 24: more leaves and
  misses), and it points into itself while short, so it shifts one object
  at a time where Rust moves bytes (PORTING.md, [moves](../../PORTING.md#p6-moves-relocate-only-trivially-relocatable-types-as-bytes)). Moving strings as bytes with a
  pointer fix-up made 100k maps of 32 strings 26% faster to build in a
  prototype, at the cost of copying the bytes of a non-trivially-copyable
  type and depending on libstdc++'s layout: FUTURE_WORK.md.
  `bbtree::compact_string` keys, 16 bytes and moved as bytes, close the
  insert and 32-entry build gaps (the string keys' table).
- **Bulk builds (1.3×), closed:** the build loop read its state back
  from members after every store into a page, which C++ can't rule out
  aliasing and Rust's `&mut self` does (PORTING.md,
  [loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)). The probe that matched the
  Rust bench rebuilt on warm pages, which hid it. Now 0.79× and 0.98×
  (the table above).
- **Iterating 1M maps of 4 entries (1.8×), closed:** the walk read every
  entry of a leaf with one load, whose steps no stride prefetcher follows;
  Rust's fold, vectorized four entries a round, reads each with one of its
  own. Four entries a round in a root leaf ([iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)): `compact_string` keys
  0.64-0.71× the Rust crate's `String`, `std::string` keys 1.0-1.2×.
- **The store's scan (1.5× → 1.13×):** the scan's visit and the record's
  `value()` were calls, a third more instructions an entry, so fewer
  entries' record misses overlapped ([store scans](../../PORTING.md#p31-a-store-scans-visit-inlines-into-the-maps-loop)). What is left is bbtree++'s 7
  more heap bytes an entry (87 against 80; [heads' records](../../PORTING.md#p24-a-leafs-head-keys-record-stays-put)). Long values (1.1-1.3×)
  are not looked into.

Everything else is within 10% or faster, mostly from bbtree++'s lookups
(`get_hit`/`get_miss` 0.63-0.99×) and the upsert path.
