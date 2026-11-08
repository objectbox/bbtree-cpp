# bbtree++ vs. bbtree (Rust) vs. standard maps

This C++ port of bbtree (bbtree++) was measured against the Rust original and standard maps.
Both ran the same benchmark cells with the same inputs on one machine; see Setup for details.

## Summary

- **Integer keys:** bbtree++ is typically 1.0-1.35× as fast, up to 1.6×,
  and within 3.5% where it is slower, but for one walk whose reruns read
  even (†).
- **String keys:** with compact strings bbtree++ is typically 1.15-1.45×
  as fast, up to 1.7×, and even on building many 4-entry maps.
- **The shared map:** faster on one thread, even with 8 threads.
- **bbtree-store:** inserts and gets are even or faster; scans are 5% slower.

## Integer keys

The unsync map with `u64` keys and values, nanoseconds per operation
(iterate: per entry); smaller is better. The cells are the bench's `ci`
suite, its core set at 1k, 1M and 10M entries, named for running in
continuous integration. Bold marks the fastest map of each row. The two
right columns compare different maps: Rust's `BTreeMap` is a B-tree,
`std::map` a red-black tree.

| cell | bbtree++ | bbtree (Rust) | Rust `BTreeMap` | C++ `std::map` |
|---|--:|--:|--:|--:|
| u64_u64_random_insert_1k | 28.8 | 35.6 | **17.6** | 22.5 |
| u64_u64_sequential_insert_1k | **7.11** | 8.02 | 23.9 | 27.8 |
| u64_u64_get_hit_1k | **16.0** | 24.4 | 28.1 | 23.6 |
| u64_u64_get_miss_1k | **14.9** | 23.8 | 28.1 | 23.3 |
| u64_u64_iterate_1k | **0.211** | 0.332 | 1.02 | 3.02 |
| u64_u64_range64_1k | **99.1** | 120 | 102 | 252 |
| u64_u64_random_remove_1k | **38.1** | 44.5 | 48.2 | 52.0 |
| u64_u64_entry_upsert_1k | **28.8** | 46.1 | 37.5 | 38.3 |
| u64_u64_pop_first_1k | 14.2 | 14.4 | 17.7 | **8.83** |
| u64_u64_bulk_build_1k | 2.82 | 3.80 | **2.56** | 31.7 |
| u64_u64_random_insert_1M | **86.5** | 88.6 | 101 | 199 |
| u64_u64_sequential_insert_1M | 10.5 | **10.3** | 46.2 | 79.5 |
| u64_u64_get_hit_1M † | **90.8** | 94.3 | 146 | 207 |
| u64_u64_get_miss_1M | **63.5** | 79.0 | 143 | 190 |
| u64_u64_iterate_1M † | 0.785 | **0.718** | 3.50 | 52.2 |
| u64_u64_range64_1M | **234** | 253 | 378 | 3152 |
| u64_u64_random_remove_1M | **92.4** | 100 | 117 | 276 |
| u64_u64_entry_upsert_1M | **71.5** | 91.1 | 98.8 | 198 |
| u64_u64_pop_first_1M | **16.8** | 20.4 | 20.2 | 43.0 |
| u64_u64_bulk_build_1M | 5.19 | 5.54 | **4.62** | 87.4 |
| u64_u64_random_insert_10M | 248 | **240** | 261 | 687 |
| u64_u64_get_hit_10M | 245 | **238** | 311 | 468 |
| u64_u64_get_miss_10M | **172** | 209 | 315 | 471 |

† Reruns of these two cells, of this build and of one without the
second round's changes, ranged from 79 to 106 ns and from 0.60 to 0.92 ns
an entry, for the same instructions: read them as even.

## String keys

The unsync map, nanoseconds per operation (iterate and many maps: per
entry); smaller is better. bbtree++
runs each string cell with the compact string that is fastest on it,
named in the row: `compact_string` (16 bytes, a 4-byte size, up to 12
bytes in place, the default), `compact_string<2>` (16 bytes, a 2-byte
size, 14 in place: short for `basic_compact_string<std::uint16_t, 14>`,
which has no alias) or `compact_string_24` (24 bytes, 20 in place);
CPP_MAPS.md, [compact strings' variants](CPP_MAPS.md#string-key-types), has every variant on every cell.
The other maps have their language's standard string: libstdc++'s 32-byte
`std::string` (bbtree++ too, in the `std::string` column) and the Rust
crate's 24-byte `String`. Variants within 3% of each other are within
run-to-run noise. Bold marks the fastest map of each row.

| cell | bbtree++ | compact string | bbtree++ `std::string` | bbtree (Rust) | Rust `BTreeMap` | C++ `std::map` |
|---|--:|---|--:|--:|--:|--:|
| str-16_u64_random_insert_1k | **38.9** | `compact_string<2>` | 58.8 | 48.9 | 78.2 | 108 |
| str-16_u64_get_hit_1k | **32.5** | `compact_string_24` | 37.8 | 44.0 | 63.5 | 82.9 |
| str-16_u64_get_miss_1k | **23.0** | `compact_string_24` | 23.4 | 30.2 | 77.8 | 76.4 |
| str-json_u64_random_insert_1M | **133** | `compact_string<2>` | 226 | 181 | 296 | 457 |
| str-json_u64_get_hit_1M | **194** | `compact_string_24` | 228 | 264 | 478 | 638 |
| str-json_u64_get_miss_1M | **142** | `compact_string<2>` | 159 | 209 | 497 | 618 |
| str-json_struct32_build_many_1Mx4 | 61.9 | `compact_string<2>` | 73.3 | 60.3 | **39.1** | 51.5 |
| str-json_struct32_get_many_1Mx4 | 75.9 | `compact_string_24` | **74.4** | 79.8 | 98.0 | 100 |
| str-json_struct32_iterate_1Mx4 | **3.31** | `compact_string<2>` | 5.40 | 4.86 | 6.11 | 4.51 |
| str-json_struct32_build_many_100kx32 | **53.0** | `compact_string<2>` | 94.6 | 61.0 | 55.9 | 86.9 |
| str-json_struct32_get_many_100kx32 | **53.8** | `compact_string<2>` | 62.3 | 67.5 | 62.1 | 85.6 |
| str-json_struct32_iterate_100kx32 | **5.20** | `compact_string<2>` | 7.23 | 8.91 | 10.4 | 25.4 |

The `1Mx4` and `100kx32` rows are the `tiny` suite's: a million maps of 4
entries and 100k maps of 32, keys and 32-byte values.

With compact keys bbtree++ inserts, looks up, iterates and builds 32-entry
maps faster than the Rust crate, and builds many 4-entry maps about as
fast, though each map takes a full 1 KiB root page (FUTURE_WORK.md). With
`std::string` keys it looks up faster and iterates as fast, and inserts and
builds slower (see "Where bbtree++ trails"). Iterating maps of 4 trailed
until the root leaf's entries went four a round (PORTING.md,
[iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)): one load per entry, stepping by a page from map to map, which
the prefetcher follows.

## The shared map, u64 → u64

The benches run the shared map at 1k only; the 8-thread rows come from the
same probe in both languages (1M scrambled keys, 8 threads, wall time an
operation; `bbtree::bbtree_map` against `bbtree::BBTreeMap`, both on
mimalloc).

Bold marks the faster of each pair: shared against shared, unsync
against unsync.

| u64 → u64, 1k | bbtree++ shared | bbtree (Rust) shared | bbtree++ unsync | bbtree (Rust) unsync |
|---|--:|--:|--:|--:|
| random_insert | **33.4** | 37.7 | **28.8** | 35.6 |
| sequential_insert | **8.33** | 11.9 | **7.26** | 8.02 |
| get_hit | **18.6** | 25.3 | **15.8** | 24.4 |
| get_miss | **16.8** | 23.4 | **14.8** | 23.8 |
| iterate | **0.589** | 0.819 | **0.215** | 0.332 |
| range64 | **73.9** | 100 | **98.8** | 120 |
| random_remove | **43.4** | 47.5 | **36.6** | 44.5 |

| 1M, 8 threads | bbtree++ | bbtree (Rust) |
|---|--:|--:|
| insert | 44 | **42** |
| get_copied | **52** | 56 |
| update | **21** | **21** |
| remove | 40 | **39** |

## bbtree-store, `examples/bench` at 1M entries

Bold marks the smaller figure of each row.

| | bbtree++ | bbtree (Rust) |
|---|--:|--:|
| 1 thread: insert, ns | **250** | 288 |
| 1 thread: get, ns † | **247** | 254 |
| 1 thread: scan, ns per entry | 20.6 | **19.6** |
| nodes + heap, bytes per entry | 110.6 | **103.6** |
| churn, heap bytes a live record byte (round 4) | 1.74 | **1.52** |
| 8 threads: insert, ns wall | **66** | 72 |
| 8 threads: get, ns wall | **38** | 44 |
| 4,000-byte values: insert / get, ns | **1751** / 727 | 1856 / **602** |

† 221 and 227 ns on two reruns of bbtree++'s right after.

The store keeps a head's record in place (the race TSan found; PORTING.md, [heads' records](../../PORTING.md#p24-a-leafs-head-keys-record-stays-put)), which
costs up to 8 more bytes a record and the stubs churn leaves: the bytes
and churn rows.

## Setup

- **Machine:** AMD Ryzen 9 5950X (Zen 3, AVX2, no AVX-512), Linux 6.8,
  quiet (load under 0.3 at the start of each run, no compaction stall in
  the kernel during any), measured on 2026-11-08.
- **Rust:** the original at `3ab503d`, rustc 1.99.0, its bench as shipped
  (mimalloc crate 0.1, which builds mimalloc 3).
- **C++:** Clang 15, `--preset bench` (`-O3 -march=native`, LTO), mimalloc
  3.3.2 (PORTING.md, [the bench's allocator](../../PORTING.md#p17-the-benchs-allocator-mimalloc-332-behind-operator-new)).
- **Inputs:** the same keys, values, orders and probes: `bbtree-bench
  self-check` pins the C++ bench's generators to the Rust bench's (PORTING.md,
  [bench inputs](../../PORTING.md#p19-bench-inputs-the-rust-benchs-key-for-key)).
- **Runs:** one per cell, each in a fresh process (AGENTS.md: no repeats to
  average noise away), in one window: the Rust bench's `ci`, `tiny` and
  `small --key u64 --size 1k --map bbtree,bbtree-unsync`; the C++ bench's
  `small tiny cpp --key u64,str-16,str-json` on every map,
  `cpp --key str-16,str-json --size 1k,1M,1Mx4,100kx32` on bbtree++'s
  string key types (the bbtree++ string columns) and the shared map's
  `small` cells; the 8-thread probe and the store bench once each. Those
  two predate the unsync map's appends checking the shared prefix out of
  line ([loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals));
  they run the shared map, which that change left as it was.
- **Builds:** between two builds of the bench, a cell can move by up to
  13% in cycles for the same instructions, from where the linker places its
  loops (`range64` and 1k random inserts, counted with `perf stat`; `u64`
  walks moved as much in time). A difference of that size between builds,
  not between runs of one build, can be that, not the change between them.
- **Reproduce:** `bbtree-bench ci --out A` in each bench, then
  `bbtree-bench compare --head A/cpp.csv --base A/rust.csv`.

## What the first optimization round changed

| change | effect |
|---|---|
| huge pages in the bench, as the Rust bench's mimalloc has (PORTING.md, [bench dependencies](../../PORTING.md#p12-bench-dependencies-include-mimalloc), [the allocator](../../PORTING.md#p17-the-benchs-allocator-mimalloc-332-behind-operator-new)) | iteration 2× faster: most of the first measurement's gap |
| `for_each`, a depth-first walk with no cursor ([iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)) | 1M u64 walk 0.94 → 0.56 ns per entry, 1k 0.69 → 0.25, 1M maps of 4: 18 → 8 |
| one move per shifted object, not two through a temporary ([moves](../../PORTING.md#p6-moves-relocate-only-trivially-relocatable-types-as-bytes)) | 1M `std::string` inserts 252 → 221 ns |
| bulk builds: sorted ranges build in place; a leaf's run appends without the prefix shrink, its state in locals ([loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)) | 1M u64 bulk build 8.1 → 5.5 ns |
| shared map appends check the last leaf before latching it, as the Rust crate does (a porting omission) | 8-thread inserts 110 → 45 ns; the store's 143 → 67 |
| a root leaf walked with no call, four entries a round ([iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)) | 1M maps of 4: 7.2-9.0 → 5.4 ns per entry with `std::string` keys |
| the store scan's visit inlined ([store scans](../../PORTING.md#p31-a-store-scans-visit-inlines-into-the-maps-loop)) | 28.5 → 23.0 ns per entry |

## What the second round changed

The changes after the comparison with stx_btree (CPP_MAPS.md, against
stx_btree): short byte strings' windows from two overlapping loads
(PORTING.md, [short windows](../../PORTING.md#p35-a-short-byte-strings-window-overlapping-loads-not-a-copy)), an iterator step inlined whole, so that the position
stays in registers ([steps](../../PORTING.md#p38-a-step-is-inlined-whole-so-the-position-stays-in-registers)), and the shared map's scratch arrays left
unwritten until used ([scratch arrays](../../PORTING.md#p37-the-shared-maps-scratch-arrays-stay-unwritten-until-used)). Before and after, in one window of
their own, one run each, before the appends' fix below; nanoseconds per
operation.

| cell | before | after |
|---|--:|--:|
| u64_u64_range64_1k | 108 | 95.1 |
| u64_u64_range64_1M | 253 | 237 |
| str-16_u64_range64_1k, `std::string` keys | 246 | 150 |
| str-16_u64_range64_1k, `compact_string` keys | 195 | 124 |
| str-json_u64_get_hit_1M, `std::string` keys | 266 | 240 |
| str-json_u64_get_miss_1M, `std::string` keys | 178 | 156 |
| shared map: u64_u64_iterate_1k (a scan) | 0.806 | 0.603 |
| shared map: u64_u64_range64_1k | 87.3 | 77.7 |
| shared map: u64_u64_sequential_insert_1k | 10.1 | 8.58 |

Appends check the shared prefix out of line (PORTING.md, [loops that
store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)), which the bulk builds' change had inlined into the insert
path: 1k sequential `u64` inserts 8.1-8.6 → 7.3-7.4 ns and 1M 11.4-11.8 →
9.8-10.5, in interleaved runs. Range-for over a whole map, a probe: 1k `u64` entries 1.34 → 0.70 ns an
entry, backward 1.37 → 1.00. The shared map's `contains`, on a probe of 1M
`u64` keys, settles unlatched where `get_copied` does ([`contains`](../../PORTING.md#p41-contains-settles-unlatched-where-get_copied-does)):
hits 145 → 100 ns and misses 157 → 90 single-threaded, 20 → 13 ns wall at
8 threads.

## Where bbtree++ trails, and why

- **`std::string` inserts (1.2-1.25×), small string maps' builds
  (1.2-1.55×):** libstdc++'s `std::string` is 32 bytes against `String`'s
  24 (19 entries a `std::string` → `u64` leaf against 24: more leaves and
  misses), and it points into itself while short, so it shifts one object
  at a time where Rust moves bytes (PORTING.md, [moves](../../PORTING.md#p6-moves-relocate-only-trivially-relocatable-types-as-bytes)). Moving strings as bytes with a
  pointer fix-up made 100k maps of 32 strings 26% faster to build in a
  prototype, at the cost of copying the bytes of a non-trivially-copyable
  type and depending on libstdc++'s layout: FUTURE_WORK.md.
  `bbtree::compact_string` keys, 16 bytes and moved as bytes, close the
  insert and build gaps (the string keys' table).
- **Sequential inserts (1.06-1.14×), closed:** each append checks the key
  against the prefix every key shares, and the check by 8-byte windows that
  came with the bulk builds' change, inlined into the insert path, took
  registers from it: the leaf's length was spilled and loaded straight
  back. Out of line ([loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)), 0.89-1.02×.
- **Bulk builds (1.3×), closed:** the build loop read its state back
  from members after every store into a page, which C++ can't rule out
  aliasing and Rust's `&mut self` does (PORTING.md,
  [loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)). The probe that matched the
  Rust bench rebuilt on warm pages, which hid it. Now 0.74-0.94×.
- **Iterating 1M maps of 4 entries (1.8×), closed:** the walk read every
  entry of a leaf with one load, whose steps no stride prefetcher follows;
  Rust's fold, vectorized four entries a round, reads each with one of its
  own. Four entries a round in a root leaf ([iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)): `compact_string` keys
  0.68× the Rust crate's `String`, `std::string` keys 1.0-1.1× across runs.
- **The store's scan (1.5× → 1.05×):** the scan's visit and the record's
  `value()` were calls, a third more instructions per entry, so fewer
  entries' record misses overlapped ([store scans](../../PORTING.md#p31-a-store-scans-visit-inlines-into-the-maps-loop)). What is left is bbtree++'s 7
  more heap bytes per entry (87 against 80; [heads' records](../../PORTING.md#p24-a-leafs-head-keys-record-stays-put)). Gets of
  long values (1.2×) are not looked into.

Everything else is within 10% or faster, mostly from bbtree++'s lookups
(`get_hit`/`get_miss` 0.63-1.03×) and the upsert path.
