# bbtree++ vs. bbtree (Rust) vs. standard maps

This C++ port of bbtree (bbtree++) was measured against the Rust original and standard maps.
Both ran the same benchmark cells with the same inputs on one machine; see Setup for details.
[COMPARISON_HISTORY.md](COMPARISON_HISTORY.md) has what each round of optimization changed.

## Summary

- **Integer keys:** bbtree++ is typically 1.15-2× as fast, up to 2.7× on
  1k lookups, and even on `pop_first` and on 10M random inserts.
- **String keys:** with compact strings bbtree++ is 1.15-1.7× as fast. Key
  type for key type, `std::string` against the Rust crate's `String`, it is
  1.1-1.55× as fast on lookups and 0.66-0.94× on inserts and builds.
- **The shared map:** 1.2-2.2× as fast on one thread; with 8 threads
  inserts, updates and removes are 1.1-1.2× as fast and gets even.
- **bbtree-store:** on one thread inserts and gets are 1-7% slower, on
  eight even; scans are 1.25× slower and gets of long values 1.15-1.3×
  slower.

## Integer keys

The unsync map with `u64` keys and values, nanoseconds per operation
(iterate: per entry); smaller is better. The cells are the bench's `ci`
suite, its core set at 1k, 1M and 10M entries, named for running in
continuous integration. Bold marks the fastest map of each row. The two
right columns compare different maps: Rust's `BTreeMap` is a B-tree,
`std::map` a red-black tree.

| cell | bbtree++ | bbtree (Rust) | Rust `BTreeMap` | C++ `std::map` |
|---|--:|--:|--:|--:|
| u64_u64_random_insert_1k | 23.5 | 36.4 | **17.9** | 21.4 |
| u64_u64_sequential_insert_1k | **6.44** | 8.03 | 23.4 | 27.8 |
| u64_u64_get_hit_1k | **9.54** | 25.1 | 28.2 | 23.0 |
| u64_u64_get_miss_1k | **8.95** | 23.9 | 28.4 | 23.8 |
| u64_u64_iterate_1k | **0.214** | 0.424 | 0.992 | 3.05 |
| u64_u64_range64_1k | **84.0** | 123 | 104 | 255 |
| u64_u64_random_remove_1k | **33.2** | 45.8 | 48.6 | 51.7 |
| u64_u64_entry_upsert_1k | **23.8** | 46.4 | 37.6 | 36.9 |
| u64_u64_pop_first_1k | 14.4 | 14.6 | 17.6 | **9.84** |
| u64_u64_bulk_build_1k | **2.05** | 3.85 | 2.46 | 30.7 |
| u64_u64_random_insert_1M | **79.5** | 93.2 | 103 | 198 |
| u64_u64_sequential_insert_1M | **9.41** | 10.9 | 47.3 | 80.1 |
| u64_u64_get_hit_1M | **74.2** | 96.4 | 158 | 210 |
| u64_u64_get_miss_1M | **49.2** | 84.1 | 162 | 193 |
| u64_u64_iterate_1M | **0.630** | 0.783 | 3.44 | 53.7 |
| u64_u64_range64_1M | **224** | 253 | 439 | 3190 |
| u64_u64_random_remove_1M | **85.6** | 106 | 122 | 284 |
| u64_u64_entry_upsert_1M | **57.4** | 98.3 | 102 | 201 |
| u64_u64_pop_first_1M | **17.4** | 17.6 | 21.2 | 43.9 |
| u64_u64_bulk_build_1M | **3.90** | 5.15 | 4.75 | 90.2 |
| u64_u64_random_insert_10M | **241** | 247 | 261 | 691 |
| u64_u64_get_hit_10M | **183** | 244 | 304 | 466 |
| u64_u64_get_miss_10M | **140** | 198 | 338 | 471 |

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
| str-16_u64_random_insert_1k | **36.7** | `compact_string<2>` | 54.8 | 49.6 | 80.0 | 108 |
| str-16_u64_get_hit_1k | **29.6** | `compact_string_24` | 33.7 | 44.8 | 64.3 | 81.5 |
| str-16_u64_get_miss_1k | 19.9 | `compact_string_24` | **19.7** | 30.4 | 76.2 | 81.0 |
| str-json_u64_random_insert_1M | **132** | `compact_string` | 214 | 190 | 320 | 454 |
| str-json_u64_get_hit_1M | **189** | `compact_string_24` | 225 | 271 | 519 | 629 |
| str-json_u64_get_miss_1M | **132** | `compact_string<2>` | 149 | 211 | 559 | 655 |
| str-json_struct32_build_many_1Mx4 | 58.3 | `compact_string` | 71.2 | 66.7 | **40.1** | 46.2 |
| str-json_struct32_get_many_1Mx4 | **72.6** | `compact_string_24` | 73.4 | 81.9 | 98.3 | 99.3 |
| str-json_struct32_iterate_1Mx4 | **3.10** | `compact_string` | 5.44 | 5.09 | 6.11 | 4.78 |
| str-json_struct32_build_many_100kx32 | **49.2** | `compact_string` | 93.6 | 62.2 | 58.5 | 83.3 |
| str-json_struct32_get_many_100kx32 | **48.5** | `compact_string<2>` | 55.2 | 67.7 | 62.5 | 84.0 |
| str-json_struct32_iterate_100kx32 | **5.36** | `compact_string` | 7.58 | 9.02 | 11.1 | 24.8 |

The `1Mx4` and `100kx32` rows are the `tiny` suite's: a million maps of 4
entries and 100k maps of 32, keys and 32-byte values.

With compact keys bbtree++ inserts, looks up, iterates and builds 32-entry
maps faster than the Rust crate, and builds many 4-entry maps 1.15× as
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
| random_insert | **26.1** | 38.8 | **23.5** | 36.4 |
| sequential_insert | **8.29** | 11.6 | **6.44** | 8.11 |
| get_hit | **13.3** | 25.5 | **9.54** | 24.9 |
| get_miss | **10.7** | 23.1 | **8.95** | 24.1 |
| iterate | **0.614** | 0.825 | **0.214** | 0.338 |
| range64 | **70.8** | 102 | **84.0** | 118 |
| random_remove | **34.0** | 50.2 | **33.2** | 44.6 |

| 1M, 8 threads | bbtree++ | bbtree (Rust) |
|---|--:|--:|
| insert | **37-41** | 42-44 |
| get_copied | 49-53 | 50-54 |
| update | **18-19** | 21-23 |
| remove | **35-37** | 40-41 |

Four passes of each probe, in two runs.

## bbtree-store, `examples/bench` at 1M entries

Both built for the host CPU with link-time optimization (Setup); two runs
each. Bold marks the smaller figure of each row, where the runs' ranges
don't overlap.

| | bbtree++ | bbtree (Rust) |
|---|--:|--:|
| 1 thread: insert, ns | 231-243 | **226-229** |
| 1 thread: get, ns | 222-226 | **212-219** |
| 1 thread: scan, ns per entry | 21.6 | **16.9-17.2** |
| nodes + heap, bytes per entry | 110.6 | **103.6** |
| churn, heap bytes a live record byte (round 4) | 1.74 | **1.52** |
| 8 threads: insert, ns wall | 61-65 | 62-64 |
| 8 threads: get, ns wall | 33-34 | 34-35 |
| 4,000-byte values: insert / get, ns | 1731-1751 / 702-704 | 1737-1762 / **550-616** |

The store keeps a head's record in place (the race TSan found, which the
Rust crate has; PORTING.md, [heads' records](../../PORTING.md#p24-a-leafs-head-keys-record-stays-put)): the pages such records pin
are the bytes and churn rows. An earlier version of this table had the Rust
side built for baseline x86-64 without link-time optimization, which made
its inserts and gets 3-14% slower than they are.

## Setup

- **Machine:** AMD Ryzen 9 5950X (Zen 3, AVX2, no AVX-512), Linux 6.8,
  quiet (load under 0.3 at the start of each run, no compaction stall in
  the kernel during any), measured on 2026-11-10.
- **Rust:** the original at `3ab503d`, rustc 1.99.0 (LLVM 23), its bench as
  shipped (mimalloc crate 0.1, which builds mimalloc 3; `-C
  target-cpu=native`, fat LTO, one codegen unit). The store's example and
  the 8-thread probe are not the bench crate's and were built with the same
  three settings. The Rust bench ran about 50 minutes before the C++
  bench's runs these tables have, each in a quiet window, not back to
  back.
- **C++:** Clang 15, `--preset bench` (`-O3 -march=native`, LTO), mimalloc
  3.3.2 (PORTING.md, [the bench's allocator](../../PORTING.md#p17-the-benchs-allocator-mimalloc-332-behind-operator-new)).
- **Inputs:** the same keys, values, orders and probes: `bbtree-bench
  self-check` pins the C++ bench's generators to the Rust bench's (PORTING.md,
  [bench inputs](../../PORTING.md#p19-bench-inputs-the-rust-benchs-key-for-key)).
- **Runs:** one per cell, each in a fresh process (AGENTS.md: no repeats to
  average noise away), in one evening's quiet windows: the Rust bench's
  `ci`, `tiny` and `small --key u64 --size 1k --map bbtree,bbtree-unsync`;
  the C++ bench's `small tiny cpp --key u64,str-16,str-json` on every map,
  `cpp --key str-16,str-json --size 1k,1M,1Mx4,100kx32` on bbtree++'s
  string key types (the bbtree++ string columns) and the shared map's
  `small` cells; the 8-thread probe and the store bench twice each.
- **Builds:** between two builds of the bench, a cell can move by up to
  13% in cycles for the same instructions, from where the linker places its
  loops (`range64` and 1k random inserts, counted with `perf stat`; `u64`
  walks moved as much in time). A difference of that size between builds,
  not between runs of one build, can be that, not the change between them.
  Aligning every function to 64 bytes (`-falign-functions=64`) doesn't
  remove it: `range64` at 1k ran 109-116 ns on one build and 129-131 on
  another so aligned, where it ran 109-112 and 125-127 without.
- **Reproduce:** `bbtree-bench ci --out A` in each bench, then
  `bbtree-bench compare --head A/cpp.csv --base A/rust.csv`.

## Where bbtree++ trails, and why

- **`std::string` inserts (1.1×), small string maps' builds
  (1.05-1.5×):** libstdc++'s `std::string` is 32 bytes against `String`'s
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
- **The store's scan (1.25×):** the scan's visit and the record's
  `value()` were calls, a third more instructions per entry, so fewer
  entries' record misses overlapped; inlined ([store scans](../../PORTING.md#p31-a-store-scans-visit-inlines-into-the-maps-loop)), 28 ns an
  entry became 21. The Rust crate built for the host CPU scans in 17: an
  earlier 1.05× was against a build of it for baseline x86-64. Part of the
  rest is bbtree++'s 7 more heap bytes per entry (87 against 80; [heads' records](../../PORTING.md#p24-a-leafs-head-keys-record-stays-put)).
- **The store's gets of 4,000-byte values (1.15-1.3×):** not the read path,
  which runs the Rust crate's instructions (1,324 against 1,333 a get) and
  is even with it in a program of its own (549-579 against 551-572 ns). In
  the bench's process the same instructions take 1.28× the cycles, about 8
  of a get's 66 lines of memory arriving late. Not explained.

Everything else is faster: lookups 1.3-2.7×, upserts 1.7-1.95×.
