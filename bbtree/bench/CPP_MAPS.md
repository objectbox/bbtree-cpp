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
| str-json_struct32_iterate_1Mx4 † § | 7.18 | 6.30 | 0.88 | 4.52 | 3.54 | 4.61 |
| str-json_struct32_build_many_100kx32 | 105 | 61.0 | 0.58 | 60.7 | 105 | 88.7 |
| str-json_struct32_get_many_100kx32 | 69.7 | 68.3 | 0.98 | 67.4 | 68.0 | 86.6 |
| str-json_struct32_iterate_100kx32 † | 7.47 | 5.17 | 0.69 | 8.82 | 17.4 | 25.2 |
| str-json_u64_random_insert_1M | 249 | 161 | 0.65 | 182 | 374 | 487 |
| str-json_u64_get_hit_1M | 256 | 249 | 0.97 | 278 | 408 | 740 |
| str-json_u64_get_miss_1M | 161 | 176 | **1.09** | 210 | 433 | 649 |

§ Before the walk read root leaves four entries a round, which came later: 1M maps of 4 now
iterate in 5.6 ns an entry with `std::string` keys and 3.2 with
`compact_string` keys (COMPARISON.md).

† bbtree's columns re-run after the checksum became a fold (PORTING.md, [iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)).
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
- **Iteration:** 0.88× `std::string`'s for maps of 4, 0.69× for maps of
  32, faster than the Rust crate's. A first layout with 15 bytes in
  place decoded the size in eight instructions and walked 1.2-1.4× slower;
  PORTING.md, [compact_string](../../PORTING.md#p29-compact_string-12-bytes-in-place-the-size-in-one-place), has the measurement.
- **Lookups** are within 10%, behind for `str-16` at 1k and misses at 1M:
  with 12 bytes in place, 16% of JSON keys and most `str-16` keys live on
  the heap, so a tie reads a line past the node, and `data()` branches on
  the form. Not profiled further.
- The `compact_string` map beats abseil's at every write and lookup here,
  ties it on lookups in 32-entry maps (68.3 against 68.0), and trails it on
  `range64` and the 4-entry maps.

## Compact strings' variants

`basic_compact_string<Size, ShortCapacity>` in three forms against
`std::string`, the same string cells, one run each, started at load 0.3,
with no compaction stall in the kernel during the run (the 4-entry build
cell is clean here, see above).

| cell | `std::string` | `compact_string` (16 B, 4-byte size, 12 in place) | 16 B, 2-byte size, 14 in place | `compact_string_24` (24 B, 20 in place) |
|---|--:|--:|--:|--:|
| str-16_u64_random_insert_1k | 66.4 | 37.9 | 38.7 | 40.8 |
| str-16_u64_sequential_insert_1k | 31.5 | 15.8 | 17.3 | 17.6 |
| str-16_u64_get_hit_1k | 37.1 | 37.5 | 38.0 | 32.9 |
| str-16_u64_get_miss_1k | 23.0 | 24.4 | 25.9 | 23.7 |
| str-16_u64_iterate_1k | 0.443 | 0.428 | 0.405 | 0.456 |
| str-16_u64_range64_1k | 251 | 187 | 179 | 209 |
| str-16_u64_random_remove_1k | 111 | 65.6 | 66.4 | 67.3 |
| str-16_u64_entry_upsert_1k | 71.1 | 43.0 | 42.5 | 45.5 |
| str-16_u64_pop_first_1k | 71.0 | 15.9 | 15.7 | 18.3 |
| str-16_u64_bulk_build_1k | 19.9 | 11.4 | 13.2 | 12.0 |
| str-json_struct32_build_many_1Mx4 | 77.3 | 66.1 | 66.9 | 65.5 |
| str-json_struct32_get_many_1Mx4 | 73.6 | 78.3 | 75.0 | 74.7 |
| str-json_struct32_iterate_1Mx4 § | 7.18 | 6.14 | 6.16 | 6.96 |
| str-json_struct32_build_many_100kx32 | 99.8 | 60.0 | 59.8 | 65.7 |
| str-json_struct32_get_many_100kx32 | 71.8 | 63.4 | 63.5 | 68.6 |
| str-json_struct32_iterate_100kx32 | 7.27 | 5.26 | 5.73 | 6.29 |
| str-json_u64_random_insert_1M | 251 | 159 | 156 | 175 |
| str-json_u64_get_hit_1M | 263 | 243 | 244 | 219 |
| str-json_u64_get_miss_1M | 179 | 181 | 164 | 181 |

§ Before the walk read root leaves four entries a round, which came later: 1M maps of 4 now
iterate in 5.6 ns an entry with `std::string` keys and 3.2 with
`compact_string` keys (COMPARISON.md).

- **A 2-byte size** buys 2 bytes in place: 90% of JSON keys against 84%.
  Misses in 1M maps are 9% faster, fewer ties reading past the node; the
  rest is within run-to-run noise. Strings are capped at 65,535 bytes.
- **24 bytes** hold most `str-16` keys (median 16) in place: hits 12%
  faster at 1k and 10% at 1M JSON keys. A string → `u64` leaf entry is 41
  bytes, as the Rust crate's `String`, against 33: inserts 8-11% slower,
  walks 7-20%, `pop_first` 15%.
- `compact_string` stays the 16-byte default: no cap, and nothing the
  2-byte size wins is outside noise but one cell.

## Memory, bytes an entry

The heap bytes a map holds after its build, divided by its entries:
`bbtree-bench-memory cpp`, which counts the bytes mimalloc hands out
(`mi_usable_size`) and frees, one run each. The counts don't depend on
the machine's load. bbtree's string rows have the compact string that
suits the keys (see above): `compact_string_24` (`bbtree-unsync-compact-24`,
24 bytes, up to 20 in place) for `str-16`, whose keys it mostly holds in
place, and the default `compact_string` (`bbtree-unsync-compact`, 16 bytes,
a 4-byte size, up to 12 in place) for JSON keys. The other maps have
`std::string` keys.

| cell | bbtree | absl btree | std::map | std::unordered_map | bbtree ÷ std::map |
|---|--:|--:|--:|--:|--:|
| u64_u64_random_insert_1k | 27.7 | **20.8** | 48.0 | 42.3 | 0.58 |
| u64_u64_sequential_insert_1k | 24.6 | **18.1** | 48.0 | 42.3 | 0.51 |
| u64_u64_bulk_build_1k | 19.5 | **18.1** | 48.0 | 42.3 | 0.41 |
| u64_u64_random_insert_1M | 26.9 | **21.4** | 48.0 | 44.6 | 0.56 |
| u64_u64_sequential_insert_1M | 23.6 | **17.6** | 48.0 | 44.6 | 0.49 |
| u64_u64_bulk_build_1M | 18.5 | **17.6** | 48.0 | 44.6 | 0.38 |
| u64_u64_random_insert_10M | 26.9 | **21.4** | 48.0 | 42.1 | 0.56 |
| str-16_u64_random_insert_1k | **67.2** | 74.0 | 98.0 | 92.2 | 0.69 |
| str-16_u64_sequential_insert_1k | 68.3 | **62.6** | 98.0 | 92.2 | 0.70 |
| str-16_u64_bulk_build_1k | **48.8** | 62.6 | 98.0 | 92.2 | 0.50 |
| str-json_u64_random_insert_1M | **51.0** | 58.7 | 82.7 | 79.3 | 0.62 |

- **std::map** pays 48 bytes for a `u64` → `u64` entry in every order: a
  node is 32 bytes of links and colour beside the entry's 16, mimalloc's
  48-byte size class. With `std::string` keys a node is 72 bytes, in the
  80-byte class, plus a heap buffer for each key past 15 bytes.
- **bbtree's** `u64` entry is its 16 bytes and a 2-byte inline key: 18.5
  bytes in the full leaves of a bulk build, 23.6 after ascending inserts,
  26.9 after random ones, which leave leaves about 70% full. abseil's
  nodes keep no inline keys and are 1-6 bytes an entry smaller.
- **String keys:** a string → `u64` leaf entry is 33 bytes with
  `compact_string`, 41 with `compact_string_24` and 49 with `std::string`.
  On `str-16` the 24-byte form saves the heap buffers of the keys it holds
  in place: 67.2 against the 16-byte form's 71.6 after random inserts and
  48.8 against 55.2 in a bulk build, but 68.3 against 65.5 after
  sequential inserts. On JSON keys it takes 61.5 against 51.0. The
  2-byte-size variant is 1-2.4 bytes smaller than `compact_string` on
  every cell. With `std::string` keys bbtree takes
  76.9 bytes an entry on 1M JSON keys (0.93× std::map) and 72-106 on
  `str-16` at 1k.
- **The Rust crate** takes the same bytes on every `u64` cell; with its
  24-byte `String` keys it takes 68.4 on 1M JSON keys.

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
  - many 4-entry maps: 1.6× to build, 2.4× to iterate; to iterate 1.6×
    since the walk reads root leaves four entries a round, which came after
    these tables
    (bbtree 5.6 ns an entry, `compact_string` keys 3.2; COMPARISON.md).
- **About even:** removes, bulk builds, lookups in many small maps.
- **At 1M and 10M:** bbtree misses 1.75-1.85× faster on u64 and 2.5× on
  JSON strings, hits 1.4-1.6× faster at 10M u64 and on strings, inserts
  JSON strings 1.5× and sequential u64 3.4× faster, and iterates 2.2×
  faster. abseil pops the first entry 2.1× faster and scans `range64`
  1.16× faster; random u64 inserts, removes, upserts and bulk builds are
  within 11%.

The Rust original has the 1k insert gap too: COMPARISON.md has Rust's
bbtree at 36.0 ns on 1k random u64 inserts against `BTreeMap`'s 17.5, so
that gap comes from the design, not the port. Of the many-map gaps,
iteration was profiled: the walk read a leaf's entries with one load,
whose steps the prefetcher didn't follow (since fixed; PORTING.md, [iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)). Building them
pays a full 1 KiB root page a map, where abseil sizes its root leaf to its
entries (FUTURE_WORK.md). No Rust `BTreeMap` figures for the many-map
cells are at hand.

`std::unordered_map` beats every ordered map on lookups, random inserts
and upserts, as expected; not on sequential inserts, which bbtree appends
to its last leaf, or on 1M random removes.
