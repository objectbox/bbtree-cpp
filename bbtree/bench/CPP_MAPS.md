# bbtree++ against C++ maps

`bbtree::unsync::bbtree_map` against `absl::btree_map`, `std::map` and
`std::unordered_map` (the bench's `std-hash`). COMPARISON.md compares
the port with the Rust original; this file compares it with other C++ maps.

- **Machine and build:** as in COMPARISON.md (Ryzen 9 5950X, Clang 15,
  `--preset bench`, mimalloc 3.3.2 with huge pages), abseil 20250814.2.
- **Run:** `bbtree-bench small tiny --key u64,str-16,str-json`, every map,
  122 cells, one run each in a fresh process, 21 s, load under 0.3.
- **Sizes:** `small` runs every map at 1k entries and `tiny` on many small
  maps; `cpp`, the C++ bench's own suite, runs `ci`'s 1M and 10M workloads
  on every map (`ci` itself runs only bbtree and `std::map`, as in the Rust
  bench).

## 1k entries and many small maps, ns an operation (iterate, many maps: an entry)

Bold marks the fastest ordered map. The hash map has no order, so it has
no `range64` or `pop_first`.

| cell | bbtree | absl btree | std::map | std::unordered_map |
|---|--:|--:|--:|--:|
| u64_u64_random_insert_1k | 29.8 | **17.2** | 22.6 | 10.7 |
| u64_u64_sequential_insert_1k | **7.30** | 15.9 | 26.4 | 10.1 |
| u64_u64_get_hit_1k | **15.1** | 22.1 | 24.0 | 7.51 |
| u64_u64_get_miss_1k | **14.6** | 22.0 | 22.5 | 10.6 |
| u64_u64_iterate_1k | **0.254** | 0.802 | 3.17 | 0.859 |
| u64_u64_range64_1k | 130 | **87.8** | 265 | — |
| u64_u64_random_remove_1k | **37.9** | 38.1 | 51.0 | 12.0 |
| u64_u64_entry_upsert_1k | **29.2** | 32.8 | 39.3 | 15.2 |
| u64_u64_pop_first_1k | 14.5 | **7.67** | 8.88 | — |
| u64_u64_bulk_build_1k | **4.57** | 4.68 | 31.3 | 10.2 |
| str-16_u64_random_insert_1k | **62.7** | 109 | 106 | 21.3 |
| str-16_u64_sequential_insert_1k | **32.5** | 49.0 | 118 | 26.1 |
| str-16_u64_get_hit_1k | **35.7** | 69.6 | 83.5 | 20.8 |
| str-16_u64_get_miss_1k | **22.6** | 75.4 | 78.8 | 21.4 |
| str-16_u64_iterate_1k | **0.541** | 1.24 | 3.57 | 2.23 |
| str-16_u64_range64_1k | 223 | **159** | 319 | — |
| str-16_u64_random_remove_1k | **110** | 111 | 111 | 25.5 |
| str-16_u64_entry_upsert_1k | **70.4** | 110 | 104 | 32.9 |
| str-16_u64_pop_first_1k | 73.2 | 29.7 | **15.6** | — |
| str-16_u64_bulk_build_1k | 20.1 | **19.2** | 127 | 22.2 |
| str-json_struct32_build_many_1Mx4 | 77.5 | **48.9** | 49.7 | 42.1 |
| str-json_struct32_get_many_1Mx4 | **73.3** | 75.3 | 101 | 57.2 |
| str-json_struct32_iterate_1Mx4 | 8.47 | **3.46** | 4.30 | 4.22 |
| str-json_struct32_build_many_100kx32 | 98.6 | 103 | **87.3** | 51.3 |
| str-json_struct32_get_many_100kx32 | 68.2 | **65.7** | 83.6 | 47.1 |
| str-json_struct32_iterate_100kx32 | **7.33** | 17.1 | 24.9 | 21.4 |

## 1M and 10M entries, ns an operation (iterate: an entry)

`bbtree-bench cpp`, 76 cells, once each, 81 s, started at load 0.1 after
the 5-minute load average had stayed under 0.3. Two earlier runs, on a
machine busy with an IDE's indexing, are discarded.

| cell | bbtree | absl btree | std::map | std::unordered_map |
|---|--:|--:|--:|--:|
| u64_u64_random_insert_1M | **95.9** | 98.9 | 211 | 59.7 |
| u64_u64_sequential_insert_1M | **9.83** | 33.9 | 78.7 | 61.0 |
| u64_u64_get_hit_1M † | 129 | **111** | 213 | 28.6 |
| u64_u64_get_miss_1M | **67.4** | 118 | 214 | 27.4 |
| u64_u64_iterate_1M | **0.753** | 1.68 | 61.3 | 30.8 |
| u64_u64_range64_1M | 256 | **221** | 3360 | — |
| u64_u64_random_remove_1M | 95.5 | **90.9** | 325 | 97.4 |
| u64_u64_entry_upsert_1M | **81.9** | 87.3 | 226 | 45.9 |
| u64_u64_pop_first_1M | 16.9 | **8.01** | 43.3 | — |
| u64_u64_bulk_build_1M | 7.12 | **6.44** | 88.4 | 57.4 |
| u64_u64_random_insert_10M | 264 | **250** | 703 | 197 |
| u64_u64_get_hit_10M | **255** | 351 | 499 | 39.0 |
| u64_u64_get_miss_10M | **174** | 320 | 513 | 41.8 |
| str-json_u64_random_insert_1M | **254** | 389 | 481 | 153 |
| str-json_u64_get_hit_1M | **274** | 432 | 689 | 153 |
| str-json_u64_get_miss_1M | **168** | 413 | 812 | 117 |

† bbtree's runs of this cell ranged from 88 to 129 ns today (93.4 in
COMPARISON.md; 110 on a rerun of the cell alone, at load 0.8); abseil's
from 111 to 146. Unsettled: read it as about even.

## `compact_string` keys against `std::string` keys

`bbtree-unsync-compact` is bbtree's unsync map with `bbtree::compact_string`
keys (PORTING.md, [compact_string](../../PORTING.md#p29-compact_string-12-bytes-in-place-the-size-in-one-place)): 16 bytes, up to 12 in place, moved as bytes. The
cell turns its keys into compact strings before it times anything.
`bbtree-bench cpp --key str-16,str-json --map
bbtree-unsync,bbtree-unsync-compact,absl-btree,std-btree`, one run each,
started at load 0.3, with the root-leaf walk (PORTING.md, [iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)). Rust is the Rust
crate's bbtree with `String` keys, from COMPARISON.md.

| cell | `std::string` | `compact_string` | compact ÷ std | Rust | absl btree | std::map |
|---|--:|--:|--:|--:|--:|--:|
| str-16_u64_random_insert_1k | 58.9 | 39.1 | 0.66 | 46.6 | 106 | 105 |
| str-16_u64_sequential_insert_1k | 32.1 | 16.1 | 0.50 | | 47.9 | 116 |
| str-16_u64_get_hit_1k | 36.1 | 39.4 | **1.09** | 44.5 | 71.5 | 84.4 |
| str-16_u64_get_miss_1k | 22.9 | 25.1 | **1.10** | 29.8 | 75.6 | 81.0 |
| str-16_u64_iterate_1k | 0.540 | 0.465 | 0.86 | | 1.29 | 3.70 |
| str-16_u64_range64_1k | 249 | 188 | 0.76 | | 168 | 331 |
| str-16_u64_random_remove_1k | 107 | 67.0 | 0.62 | | 110 | 113 |
| str-16_u64_entry_upsert_1k | 70.6 | 43.5 | 0.62 | | 115 | 101 |
| str-16_u64_pop_first_1k | 71.9 | 15.0 | 0.21 | | 28.9 | 15.8 |
| str-16_u64_bulk_build_1k | 21.6 | 11.4 | 0.53 | | 19.2 | 129 |
| str-json_struct32_build_many_1Mx4 ‡ | 236 | 139 | 0.59 | 61.0 | 47.6 | 50.9 |
| str-json_struct32_get_many_1Mx4 | 74.0 | 79.5 | 1.07 | 82.2 | 76.0 | 98.4 |
| str-json_struct32_iterate_1Mx4 | 7.95 | 8.04 | 1.01 | 4.52 | 3.54 | 4.61 |
| str-json_struct32_build_many_100kx32 | 105 | 61.0 | 0.58 | 60.7 | 105 | 88.7 |
| str-json_struct32_get_many_100kx32 | 69.7 | 68.3 | 0.98 | 67.4 | 68.0 | 86.6 |
| str-json_struct32_iterate_100kx32 | 7.40 | 5.41 | 0.73 | 8.82 | 17.4 | 25.2 |
| str-json_u64_random_insert_1M | 249 | 161 | 0.65 | 182 | 374 | 487 |
| str-json_u64_get_hit_1M | 256 | 249 | 0.97 | 278 | 408 | 740 |
| str-json_u64_get_miss_1M | 161 | 176 | **1.09** | 210 | 433 | 649 |

‡ Not comparable with the other runs: bbtree's maps of 4 take 1.1 GB of
fresh pages, and with the machine's memory mostly page cache, getting huge
pages stalled the kernel in compaction (29 stalls in one run of the cell,
none in abseil's, which takes a third as much). The same cell took 78 ns
with `std::string` keys earlier in the session; the two bbtree columns of
this run stalled alike.

- **Writes:** an entry of a string → `u64` leaf shrinks from 49 bytes to
  33 and shifts by one `memmove`: 1M JSON-key inserts 249 → 161 ns, below
  the Rust crate's 182; removes, upserts, bulk builds and builds of
  32-entry maps 0.5-0.66×; `pop_first` 0.21×.
- **Iteration:** at `std::string`'s speed for maps of 4, 0.73× for maps
  of 32, faster than the Rust crate's. A first layout with 15 bytes in
  place decoded the size in eight instructions and walked 1.2-1.4× slower;
  PORTING.md, [compact_string](../../PORTING.md#p29-compact_string-12-bytes-in-place-the-size-in-one-place), has the measurement.
- **Lookups** are within 10%, behind for `str-16` at 1k and misses at 1M:
  with 12 bytes in place, 16% of JSON keys and most `str-16` keys live on
  the heap, so a tie reads a line past the node, and `data()` branches on
  the form. Not profiled further.
- The `compact_string` map beats abseil's at every write and lookup here,
  ties it on lookups in 32-entry maps (68.3 against 68.0), and trails it on
  `range64` and the 4-entry maps.

## Against abseil's B-tree

- **bbtree is faster:**
  - lookups: 1.5× on u64, 2-3× on 16-byte strings;
  - string inserts and upserts: 1.6×;
  - sequential inserts: 1.5-2×;
  - iterating 1k-entry maps: 2-3×.
- **abseil is faster:**
  - random u64 inserts at 1k: 1.7×;
  - `range64`: 1.4-1.5×;
  - `pop_first`: 1.9× on u64, 2.5× on strings;
  - many 4-entry maps: 1.6× to build, 2.4× to iterate.
- **About even:** removes, bulk builds, lookups in many small maps.
- **At 1M and 10M:** bbtree misses 1.75-1.85× faster on u64 and 2.5× on
  JSON strings, hits 1.4-1.6× faster at 10M u64 and on strings, inserts
  JSON strings 1.5× and sequential u64 3.4× faster, and iterates 2.2×
  faster. abseil pops the first entry 2.1× faster and scans `range64`
  1.16× faster; random u64 inserts, removes, upserts and bulk builds are
  within 11%.

The Rust original has the 1k insert gap too: COMPARISON.md has Rust's
bbtree at 36.0 ns on 1k random u64 inserts against `BTreeMap`'s 17.5, so
that gap comes from the design, not the port. The other gaps are not
profiled here, and no Rust `BTreeMap` figures for the many-map cells are
at hand; in those cells each bbtree map is a cold root page (COMPARISON.md).

`std::unordered_map` beats every ordered map on lookups, random inserts
and upserts, as expected; not on sequential inserts, which bbtree appends
to its last leaf, or on 1M random removes.
