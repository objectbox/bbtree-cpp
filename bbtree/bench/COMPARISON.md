# bbtree++ vs. bbtree (Rust) vs. standard maps

This C++ port of bbtree (bbtree++) was measured against the Rust original and standard maps.
Both ran the same benchmark cells with the same inputs on one machine; see Setup for details.

## Summary

- **Integer keys:** bbtree++ is typically 1.0-1.3× as fast, up to 1.6×,
  and slower on sequential inserts and `range64`.
- **String keys:** with compact strings bbtree++ is typically 1.1-1.35× as
  fast, up to 1.7×, and even on building many 4-entry maps.
- **The shared map:** faster on one thread, even with 8 threads.
- **bbtree-store:** inserts and gets are even or faster; scans are 6% slower.

## Integer keys

The unsync map with `u64` keys and values, nanoseconds per operation
(iterate: per entry); smaller is better. The cells are the bench's `ci`
suite, its core set at 1k, 1M and 10M entries, named for running in
continuous integration. Bold marks the fastest map of each row. The two
right columns compare different maps: Rust's `BTreeMap` is a B-tree,
`std::map` a red-black tree.

| cell | bbtree++ | bbtree (Rust) | Rust `BTreeMap` | C++ `std::map` |
|---|--:|--:|--:|--:|
| u64_u64_random_insert_1k | 29.8 | 35.8 | **17.6** | 21.9 |
| u64_u64_sequential_insert_1k | 8.29 | **7.95** | 22.9 | 27.4 |
| u64_u64_get_hit_1k | **16.0** | 24.5 | 28.8 | 23.2 |
| u64_u64_get_miss_1k | **14.9** | 23.8 | 27.7 | 22.2 |
| u64_u64_iterate_1k | **0.251** | 0.362 | 1.04 | 3.00 |
| u64_u64_range64_1k | 125 | 116 | **105** | 253 |
| u64_u64_random_remove_1k | **39.1** | 43.7 | 48.2 | 50.6 |
| u64_u64_entry_upsert_1k | **29.1** | 46.3 | 37.3 | 37.3 |
| u64_u64_pop_first_1k | 14.2 | 15.1 | 17.5 | **9.07** |
| u64_u64_bulk_build_1k | 3.04 | 3.77 | **2.47** | 32.2 |
| u64_u64_random_insert_1M | 90.2 | **88.3** | 101 | 201 |
| u64_u64_sequential_insert_1M | 11.6 | **10.1** | 47.4 | 79.3 |
| u64_u64_get_hit_1M | **81.3** | 88.4 | 150 | 205 |
| u64_u64_get_miss_1M | **65.6** | 81.1 | 139 | 191 |
| u64_u64_iterate_1M | **0.777** | 0.800 | 2.87 | 57.0 |
| u64_u64_range64_1M | 277 | **242** | 378 | 3182 |
| u64_u64_random_remove_1M | **89.4** | 96.9 | 117 | 278 |
| u64_u64_entry_upsert_1M | **68.8** | 94.2 | 100 | 199 |
| u64_u64_pop_first_1M | **16.7** | 17.0 | 20.2 | 40.5 |
| u64_u64_bulk_build_1M | 5.30 | 5.93 | **4.71** | 91.0 |
| u64_u64_random_insert_10M | 264 | **246** | 257 | 667 |
| u64_u64_get_hit_10M | **237** | 242 | 309 | 461 |
| u64_u64_get_miss_10M | **173** | 194 | 320 | 490 |

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
| str-16_u64_random_insert_1k | **38.4** | `compact_string<2>` | 60.1 | 49.1 | 79.6 | 105 |
| str-16_u64_get_hit_1k | **33.4** | `compact_string_24` | 37.5 | 42.3 | 69.1 | 84.0 |
| str-16_u64_get_miss_1k | 24.1 | `compact_string_24` | **22.2** | 28.4 | 78.6 | 81.2 |
| str-json_u64_random_insert_1M | **147** | `compact_string` | 229 | 175 | 295 | 462 |
| str-json_u64_get_hit_1M | **198** | `compact_string_24` | 240 | 269 | 488 | 632 |
| str-json_u64_get_miss_1M | **149** | `compact_string` | 154 | 208 | 518 | 643 |
| str-json_struct32_build_many_1Mx4 | 63.4 | `compact_string` | 74.6 | 61.1 | **39.1** | 48.1 |
| str-json_struct32_get_many_1Mx4 | 75.9 | `compact_string_24` | **75.4** | 80.3 | 95.2 | 99.3 |
| str-json_struct32_iterate_1Mx4 | **3.18** | `compact_string` | 5.42 | 5.28 | 5.96 | 4.43 |
| str-json_struct32_build_many_100kx32 | **54.0** | `compact_string` | 93.4 | 60.7 | 55.3 | 81.4 |
| str-json_struct32_get_many_100kx32 | **53.1** | `compact_string<2>` | 63.2 | 66.1 | 61.5 | 86.3 |
| str-json_struct32_iterate_100kx32 | **5.19** | `compact_string<2>` | 7.29 | 8.84 | 10.3 | 25.2 |

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
| random_insert | **35.5** | 37.3 | **27.6** | 35.8 |
| sequential_insert | **8.79** | 11.5 | 8.03 | **7.95** |
| get_hit | **19.3** | 25.2 | **15.8** | 24.5 |
| get_miss | **17.8** | 23.3 | **14.6** | 23.8 |
| iterate | **0.673** | 0.872 | **0.245** | 0.362 |
| range64 | **79.6** | 100 | 120 | **116** |
| random_remove | **43.3** | 49.2 | **37.9** | 43.7 |

| 1M, 8 threads | bbtree++ | bbtree (Rust) |
|---|--:|--:|
| insert | 45 | **42** |
| get_copied | **52** | 56 |
| update | **21** | **21** |
| remove | **39** | **39** |

## bbtree-store, `examples/bench` at 1M entries

Bold marks the smaller figure of each row.

| | bbtree++ | bbtree (Rust) |
|---|--:|--:|
| 1 thread: insert, ns | **235** | 288 |
| 1 thread: get, ns | **221** | 254 |
| 1 thread: scan, ns per entry | 20.8 | **19.6** |
| nodes + heap, bytes per entry | 110.6 | **103.6** |
| churn, heap bytes a live record byte (round 4) | 1.74 | **1.52** |
| 8 threads: insert, ns wall | **66** | 72 |
| 8 threads: get, ns wall | **35** | 44 |
| 4,000-byte values: insert / get, ns | 1914 / 712 | **1856 / 602** |

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
  `small` cells; the 8-thread probe and the store bench once each.
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

## Where bbtree++ trails, and why

- **`std::string` inserts (1.2-1.3×), small string maps' builds
  (1.2-1.55×):** libstdc++'s `std::string` is 32 bytes against `String`'s
  24 (19 entries a `std::string` → `u64` leaf against 24: more leaves and
  misses), and it points into itself while short, so it shifts one object
  at a time where Rust moves bytes (PORTING.md, [moves](../../PORTING.md#p6-moves-relocate-only-trivially-relocatable-types-as-bytes)). Moving strings as bytes with a
  pointer fix-up made 100k maps of 32 strings 26% faster to build in a
  prototype, at the cost of copying the bytes of a non-trivially-copyable
  type and depending on libstdc++'s layout: FUTURE_WORK.md.
  `bbtree::compact_string` keys, 16 bytes and moved as bytes, close the
  insert and build gaps (the string keys' table).
- **`range64` (1.08-1.15×):** a scan steps with a call that keeps the
  cursor's position in memory, where the Rust crate's stays in registers
  (FUTURE_WORK.md, iterators in registers). This build also runs the
  `u64` walks and scans 7-13% slower than the previous one, for the same
  instructions: the bench binary's code placement, not its code.
- **Sequential inserts (1.04-1.15×):** each append checks the key against
  the prefix every key shares, and the check by 8-byte windows that came
  with the bulk builds' (PORTING.md, [loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)),
  inlined into the insert path, takes registers from it: an append spills
  the leaf's length and loads it straight back, though no key of these
  cells shares a byte, so the loop never runs. 1k appends 6.9 → 8.2 ns,
  where the Rust crate takes 7.95. The check out of line on the append
  path alone gave 7.3 ns and kept the build's gain; comparing bytes again
  would bring back the stall that made dense ids build at 13.7 ns an entry
  against 6.0 with the windows (PORTING.md, the same entry).
- **Bulk builds (1.3×), closed:** the build loop read its state back
  from members after every store into a page, which C++ can't rule out
  aliasing and Rust's `&mut self` does (PORTING.md,
  [loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)). The probe that matched the
  Rust bench rebuilt on warm pages, which hid it. Now 0.8-0.9×.
- **Iterating 1M maps of 4 entries (1.8×), closed:** the walk read every
  entry of a leaf with one load, whose steps no stride prefetcher follows;
  Rust's fold, vectorized four entries a round, reads each with one of its
  own. Four entries a round in a root leaf ([iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)): `compact_string` keys
  0.6× the Rust crate's `String`, `std::string` keys 1.03×.
- **The store's scan (1.5× → 1.06×):** the scan's visit and the record's
  `value()` were calls, a third more instructions per entry, so fewer
  entries' record misses overlapped ([store scans](../../PORTING.md#p31-a-store-scans-visit-inlines-into-the-maps-loop)). What is left is bbtree++'s 7
  more heap bytes per entry (87 against 80; [heads' records](../../PORTING.md#p24-a-leafs-head-keys-record-stays-put)). Gets of
  long values (1.2×) are not looked into.

Everything else is within 10% or faster, mostly from bbtree++'s lookups
(`get_hit`/`get_miss` 0.63-0.98×) and the upsert path.
