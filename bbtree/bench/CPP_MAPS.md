# bbtree++ vs. C++ maps

bbtree++'s unsync map (`bbtree::unsync::bbtree_map`) was measured against
abseil's B-tree (`absl::btree_map`), `std::map` and `std::unordered_map`.
COMPARISON.md compares it with the Rust original; see Setup for details.

All times are nanoseconds per operation (per entry for iterate and the
many-map cells), and memory is bytes per entry: smaller is better.
`std::unordered_map` is a hash map, shown for reference only: it keeps no
key order, so it iterates in no particular order and has no `range64` or
`pop_first`. The ordered maps are the real competitors.

## Summary

- **Integer keys:** bbtree++ is typically 1.5-3× as fast as `std::map` and
  the fastest ordered map on lookups, sequential inserts and walks.
- **String keys:** with its best compact string bbtree++ is typically
  1.5-3× as fast as `std::map` and abseil's B-tree, and slower mainly on
  building many 4-entry maps.
- **Memory:** bbtree++ takes 0.4-0.7× `std::map`'s bytes per entry.
- **`std::unordered_map`**, unordered and for reference only, stays faster
  on lookups and random inserts, as expected of a hash map.

## Integer keys

`u64` keys and values, nanoseconds per operation (iterate: per entry);
smaller is better. Bold marks the fastest ordered map of each row.

| cell | bbtree++ | absl btree | std::map | std::unordered_map |
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

† bbtree++'s runs of this cell ranged from 88 to 129 ns (93.4 in
COMPARISON.md; 110 on a rerun of the cell alone, at load 0.8); abseil's
from 111 to 146. Unsettled: read it as about even.

The bulk build rows predate the build's run of a leaf in locals
(PORTING.md, [loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)): COMPARISON.md has bbtree++
at 3.09 ns per entry at 1k and 5.41 at 1M since, ahead of abseil's here.

## String keys

Nanoseconds per operation (iterate and many maps: per entry); smaller is
better. bbtree++ runs each cell
with the compact string that is fastest on it, named in the row:
`compact_string` (16 bytes, a 4-byte size, up to 12 bytes in place, the
default), `compact_string<2>` (16 bytes, a 2-byte size, 14 in place: short
for `basic_compact_string<std::uint16_t, 14>`, which has no alias) or
`compact_string_24` (24 bytes, 20 in place). [String key types](#string-key-types),
below, has every variant and `std::string` on every cell. The other maps have
`std::string` keys. bbtree++'s column comes from the string key types'
run, the other maps' from the main runs (Setup). Bold marks the fastest
ordered map of each row.

| cell | bbtree++ | compact string | absl btree | std::map | std::unordered_map |
|---|--:|---|--:|--:|--:|
| str-16_u64_random_insert_1k | **37.9** | `compact_string` | 109 | 106 | 21.3 |
| str-16_u64_sequential_insert_1k | **15.8** | `compact_string` | 49.0 | 118 | 26.1 |
| str-16_u64_get_hit_1k | **32.9** | `compact_string_24` | 69.6 | 83.5 | 20.8 |
| str-16_u64_get_miss_1k | **23.7** | `compact_string_24` | 75.4 | 78.8 | 21.4 |
| str-16_u64_iterate_1k | **0.405** | `compact_string<2>` | 1.24 | 3.57 | 2.23 |
| str-16_u64_range64_1k | 179 | `compact_string<2>` | **159** | 319 | — |
| str-16_u64_random_remove_1k | **65.6** | `compact_string` | 111 | 111 | 25.5 |
| str-16_u64_entry_upsert_1k | **42.5** | `compact_string<2>` | 110 | 104 | 32.9 |
| str-16_u64_pop_first_1k | 15.7 | `compact_string<2>` | 29.7 | **15.6** | — |
| str-16_u64_bulk_build_1k | **11.4** | `compact_string` | 19.2 | 127 | 22.2 |
| str-json_struct32_build_many_1Mx4 | 65.5 | `compact_string_24` | **48.9** | 49.7 | 42.1 |
| str-json_struct32_get_many_1Mx4 | **74.7** | `compact_string_24` | 75.3 | 101 | 57.2 |
| str-json_struct32_iterate_1Mx4 § | 6.14 | `compact_string` | **3.46** | 4.30 | 4.22 |
| str-json_struct32_build_many_100kx32 | **59.8** | `compact_string<2>` | 103 | 87.3 | 51.3 |
| str-json_struct32_get_many_100kx32 | **63.4** | `compact_string` | 65.7 | 83.6 | 47.1 |
| str-json_struct32_iterate_100kx32 | **5.26** | `compact_string` | 17.1 | 24.9 | 21.4 |
| str-json_u64_random_insert_1M | **156** | `compact_string<2>` | 389 | 481 | 153 |
| str-json_u64_get_hit_1M | **219** | `compact_string_24` | 432 | 689 | 153 |
| str-json_u64_get_miss_1M | **164** | `compact_string<2>` | 413 | 812 | 117 |

§ Before the walk read root leaves four entries a round: 1M maps of 4 now
iterate in 3.2 ns per entry with `compact_string` keys (COMPARISON.md),
ahead of abseil's 3.46.

The `1Mx4` and `100kx32` rows are the `tiny` suite's: a million maps of 4
entries and 100k maps of 32, keys and 32-byte values.

## String key types

`basic_compact_string<Size, ShortCapacity>` in three forms against
`std::string` keys in bbtree++'s unsync map, nanoseconds per operation
(iterate and many maps: per entry); smaller is better. A compact string is 16 or 24 bytes and moves as
bytes (PORTING.md, [compact_string](../../PORTING.md#p29-compact_string-12-bytes-in-place-the-size-in-one-place)); each cell turns its keys into
compact strings before it times anything. Bold marks the fastest of each
row.

| cell | `std::string` | `compact_string` (16 B, 4-byte size, 12 in place) | `compact_string<2>` (16 B, 2-byte size, 14 in place) | `compact_string_24` (24 B, 20 in place) |
|---|--:|--:|--:|--:|
| str-16_u64_random_insert_1k | 66.4 | **37.9** | 38.7 | 40.8 |
| str-16_u64_sequential_insert_1k | 31.5 | **15.8** | 17.3 | 17.6 |
| str-16_u64_get_hit_1k | 37.1 | 37.5 | 38.0 | **32.9** |
| str-16_u64_get_miss_1k | **23.0** | 24.4 | 25.9 | 23.7 |
| str-16_u64_iterate_1k | 0.443 | 0.428 | **0.405** | 0.456 |
| str-16_u64_range64_1k | 251 | 187 | **179** | 209 |
| str-16_u64_random_remove_1k | 111 | **65.6** | 66.4 | 67.3 |
| str-16_u64_entry_upsert_1k | 71.1 | 43.0 | **42.5** | 45.5 |
| str-16_u64_pop_first_1k | 71.0 | 15.9 | **15.7** | 18.3 |
| str-16_u64_bulk_build_1k | 19.9 | **11.4** | 13.2 | 12.0 |
| str-json_struct32_build_many_1Mx4 | 77.3 | 66.1 | 66.9 | **65.5** |
| str-json_struct32_get_many_1Mx4 | **73.6** | 78.3 | 75.0 | 74.7 |
| str-json_struct32_iterate_1Mx4 § | 7.18 | **6.14** | 6.16 | 6.96 |
| str-json_struct32_build_many_100kx32 | 99.8 | 60.0 | **59.8** | 65.7 |
| str-json_struct32_get_many_100kx32 | 71.8 | **63.4** | 63.5 | 68.6 |
| str-json_struct32_iterate_100kx32 | 7.27 | **5.26** | 5.73 | 6.29 |
| str-json_u64_random_insert_1M | 251 | 159 | **156** | 175 |
| str-json_u64_get_hit_1M | 263 | 243 | 244 | **219** |
| str-json_u64_get_miss_1M | 179 | 181 | **164** | 181 |

§ As above: since the four-a-round root leaves, 5.6 ns per entry with
`std::string` keys and 3.2 with `compact_string` keys (COMPARISON.md).

- **Writes:** a string → `u64` leaf entry shrinks from 49 bytes to 33 and
  shifts by one `memmove`: 1M JSON-key inserts 251 → 159 ns; removes,
  upserts, bulk builds and builds of 32-entry maps 0.57-0.6×; `pop_first`
  0.22×.
- **Iteration:** 0.86× `std::string`'s for maps of 4, 0.72× for maps of
  32. A first layout with 15 bytes in place decoded the size in eight
  instructions and walked 1.2-1.4× slower; PORTING.md, [compact_string](../../PORTING.md#p29-compact_string-12-bytes-in-place-the-size-in-one-place),
  has the measurement.
- **Lookups** with `compact_string` are within 7% of `std::string`'s or
  faster: with 12 bytes in place, 16% of JSON keys and most `str-16` keys
  live on the heap, so a tie reads a line past the node, and `data()`
  branches on the form. Not profiled further.
- **A 2-byte size** buys 2 bytes in place: 90% of JSON keys against 84%.
  Misses in 1M maps are 9% faster, fewer ties reading past the node; the
  rest is within run-to-run noise. Strings are capped at 65,535 bytes.
- **24 bytes** hold most `str-16` keys (median 16) in place: hits 12%
  faster at 1k and 10% at 1M JSON keys. A string → `u64` leaf entry is 41
  bytes, as the Rust crate's `String`, against 33: inserts 8-11% slower,
  walks 7-20%, `pop_first` 15%.
- `compact_string` stays the 16-byte default: no cap, and nothing the
  2-byte size wins is outside noise but one cell.

## Memory, bytes per entry

The heap bytes a map holds after its build, divided by its entries:
`bbtree-bench-memory cpp`, which counts the bytes mimalloc hands out
(`mi_usable_size`) and frees, one run each. The counts don't depend on
the machine's load. bbtree++'s string rows have the compact string that
suits the keys (see above): `compact_string_24` (`bbtree-unsync-compact-24`,
24 bytes, up to 20 in place) for `str-16`, whose keys it mostly holds in
place, and the default `compact_string` (`bbtree-unsync-compact`, 16 bytes,
a 4-byte size, up to 12 in place) for JSON keys. The other maps have
`std::string` keys.

| cell | bbtree++ | absl btree | std::map | std::unordered_map | bbtree++ ÷ std::map |
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
- **bbtree++'s** `u64` entry is its 16 bytes and a 2-byte inline key: 18.5
  bytes in the full leaves of a bulk build, 23.6 after ascending inserts,
  26.9 after random ones, which leave leaves about 70% full. abseil's
  nodes keep no inline keys and are 1-6 bytes per entry smaller.
- **String keys:** a string → `u64` leaf entry is 33 bytes with
  `compact_string`, 41 with `compact_string_24` and 49 with `std::string`.
  On `str-16` the 24-byte form saves the heap buffers of the keys it holds
  in place: 67.2 against the 16-byte form's 71.6 after random inserts and
  48.8 against 55.2 in a bulk build, but 68.3 against 65.5 after
  sequential inserts. On JSON keys it takes 61.5 against 51.0. The
  2-byte-size variant is 1-2.4 bytes smaller than `compact_string` on
  every cell. With `std::string` keys bbtree++ takes
  76.9 bytes per entry on 1M JSON keys (0.93× std::map) and 72-106 on
  `str-16` at 1k.
- **The Rust crate** takes the same bytes on every `u64` cell; with its
  24-byte `String` keys it takes 68.4 on 1M JSON keys.

## Against abseil's B-tree

String figures are bbtree++'s with its best compact string.

- **bbtree++ is faster:**
  - lookups: 1.5× on u64, 2-3× on 16-byte strings;
  - string writes (inserts, upserts, removes, bulk builds, `pop_first`):
    1.7-2.9×;
  - sequential inserts: 2-3×;
  - iterating 1k-entry maps: about 3×.
- **abseil is faster:**
  - random u64 inserts at 1k: 1.7×;
  - `range64`: 1.5× on u64, 1.1× on strings;
  - `pop_first` on u64: 1.9×;
  - building many 4-entry maps: 1.3×.
- **About even:** u64 removes and bulk builds, lookups in many small maps,
  and iterating many 4-entry maps since the four-a-round root leaves (3.2
  ns per entry against 3.46).
- **At 1M and 10M:** bbtree++ misses 1.75-1.85× faster on u64, hits 1.4×
  faster at 10M, inserts sequential u64 3.4× faster and iterates 2.2×
  faster; on JSON strings it inserts and misses 2.5× and hits 2× faster.
  abseil pops the first entry 2.1× faster and scans `range64` 1.16×
  faster; random u64 inserts, removes, upserts and bulk builds are within
  11%.

The Rust original has the 1k insert gap too: COMPARISON.md has
bbtree (Rust) at 36.0 ns on 1k random u64 inserts against `BTreeMap`'s
17.5, so that gap comes from the design, not bbtree++. Building many
small maps pays a full 1 KiB root page a map, where abseil sizes its root
leaf to its entries (FUTURE_WORK.md).

`std::unordered_map`, there for reference, beats every ordered map on
lookups, random inserts and upserts, as expected of a hash map; not on sequential inserts, which bbtree++ appends
to its last leaf, or on 1M random removes.

## Against stx_btree

[stx_btree](https://github.com/malkuth-software/stx_btree) is another C++20
form of bbtree, of its tree core and both maps (no file maps, store, sets,
entry API or bulk builds). Its unsync map ran in this bench's cells through
the bench's opt-in `stx-unsync` map (configure the bench with
`-DBBTREE_BENCH_STX_DIR=<an stx_btree checkout>`; `bbtree-bench cpp --key
u64,str-json,str-16 --map bbtree-unsync,stx-unsync`), one run each, load
under 0.3, mimalloc. Nanoseconds per operation (iterate and many maps: per entry);
smaller is better; bold marks the faster. Taken before bbtree++'s short
windows by overlapping loads (PORTING.md, [short windows](../../PORTING.md#p35-a-short-byte-strings-window-overlapping-loads-not-a-copy)),
which cut its 1M JSON-key misses by 12%.

| cell | bbtree++ | stx_btree |
|---|--:|--:|
| u64_u64_random_insert_1k | 28.0 | **25.5** |
| u64_u64_sequential_insert_1k | 8.02 | **6.79** |
| u64_u64_get_hit_1k | 15.9 | **12.8** |
| u64_u64_get_miss_1k | 14.4 | **12.3** |
| u64_u64_iterate_1k | **0.217** | 1.02 |
| u64_u64_range64_1k | 122 | **99.7** |
| u64_u64_entry_upsert_1k | **28.9** | 33.6 |
| str-16_u64_get_miss_1k | **22.8** | 25.4 |
| str-16_u64_range64_1k | 262 | **144** |
| str-json_struct32_build_many_1Mx4 | **76.1** | 137 |
| str-json_struct32_iterate_1Mx4 | **5.39** | 13.3 |
| str-json_struct32_get_many_100kx32 | 71.6 | **56.3** |
| u64_u64_random_insert_1M | 90.7 | **82.3** |
| u64_u64_get_hit_1M | 92.7 | **82.9** |
| u64_u64_get_miss_1M | 74.0 | **59.1** |
| u64_u64_iterate_1M | **0.688** | 1.48 |
| u64_u64_pop_first_1M | **17.0** | 20.3 |
| u64_u64_get_miss_10M | 176 | **164** |
| str-json_u64_get_hit_1M | 248 | **225** |

The rest of the 39 cells are within 10%. The two ports' algorithms match
nearly line for line; where stx_btree leads:

- **Lookups at 1k:** a standalone probe of both maps' `u64` lookups gives
  13.3 against 12.9 ns for hits and 12.8 against 13.0 for misses, 16 more
  instructions a hit in bbtree++; the bench's 1k cells show 20%, the
  bench's build of the lookup (not its algorithm) the likelier cause. A
  vectorized count of the leaf's last slots (55 two-byte slots leave 7 for
  a scalar tail) cut the count's instructions by a third and changed
  nothing: lookups wait on their loads, not on the count.
- **`range64`:** stx_btree's scan loop has no call and keeps its position
  in registers; bbtree++'s keeps it in memory (FUTURE_WORK.md, iterators
  in registers, which records a crossing tried here).
- **Sequential inserts:** 16 more instructions an append, spread over the
  append and the split's second search; no single cause.

What bbtree++ took from the comparison (PORTING.md): [packed pages](../../PORTING.md#p32-pooled_allocator-pages-packed-into-chunks-opt-in)
(`pooled_allocator`), [install rules](../../PORTING.md#p34-installing-find_packagebbtree-with-the-targets-own-names) checked by a moved install,
[short windows](../../PORTING.md#p35-a-short-byte-strings-window-overlapping-loads-not-a-copy) by overlapping loads, the shared scan's
[check of the levels it keeps](../../PORTING.md#p36-a-shared-scan-checks-the-levels-it-keeps-before-taking-the-next-leaf), and [unwritten scratch arrays](../../PORTING.md#p37-the-shared-maps-scratch-arrays-stay-unwritten-until-used)
in the shared map.

## Setup

- **Machine and build:** as in COMPARISON.md (Ryzen 9 5950X, Clang 15,
  `--preset bench`, mimalloc 3.3.2 with huge pages), abseil 20250814.2.
- **1k entries and many small maps:** `bbtree-bench small tiny --key
  u64,str-16,str-json`, every map, 122 cells, one run each in a fresh
  process, 21 s, load under 0.3.
- **1M and 10M entries:** `bbtree-bench cpp`, the C++ bench's own suite,
  which runs `ci`'s 1M and 10M workloads on every map (`ci` itself runs
  only bbtree++ and `std::map`, as in the Rust bench): 76 cells, once each,
  81 s, started at load 0.1 after the 5-minute load average had stayed
  under 0.3.
- **String key types:** `bbtree-bench cpp --key str-16,str-json` on
  bbtree++'s maps, one run each, started at load 0.3, with no compaction
  stall in the kernel during the run.
- **Memory:** `bbtree-bench-memory cpp`, as its section says.
