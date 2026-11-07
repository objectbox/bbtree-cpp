# bbtree++ against C++ maps

`bbtree::unsync::bbtree_map` against `absl::btree_map`, `std::map` and
`std::unordered_map` (the bench's `std-hash`). COMPARISON.md compares
the port with the Rust original; this file compares it with other C++ maps.

- **Machine and build:** as in COMPARISON.md (Ryzen 9 5950X, Clang 15,
  `--preset bench`, mimalloc 3.3.2 with huge pages), abseil 20250814.2.
- **Run:** `bbtree-bench small tiny --key u64,str-16,str-json`, every map,
  122 cells, one run each in a fresh process, 21 s, load under 0.3.
- **Sizes:** the suites run every map at 1k entries (`small`) and on many
  small maps (`tiny`). The 1M and 10M cells (`ci`) run only bbtree and
  `std::map`, as in the Rust bench, so abseil at those sizes is not measured
  here.

## ns an operation (iterate: an entry; many maps: an entry)

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

The Rust original has the 1k insert gap too: COMPARISON.md has Rust's
bbtree at 36.0 ns on 1k random u64 inserts against `BTreeMap`'s 17.5, so
that gap comes from the design, not the port. The other gaps are not
profiled here, and no Rust `BTreeMap` figures for the many-map cells are
at hand; in those cells each bbtree map is a cold root page (COMPARISON.md).

`std::unordered_map` beats every ordered map on lookups, random inserts
and upserts, as expected; not on sequential inserts, which bbtree appends
to its last leaf.
