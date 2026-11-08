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
  the fastest ordered map on lookups, sequential inserts, walks, upserts
  and bulk builds.
- **String keys:** with its best compact string bbtree++ is typically
  1.5-3× as fast as `std::map` and abseil's B-tree, and slower mainly on
  building many 4-entry maps.
- **Memory:** bbtree++ takes 0.4-0.7× `std::map`'s bytes per entry.
- **On glibc's allocator:** instead of mimalloc, bbtree++ takes 3-5× as
  long to walk and build large maps; the opt-in pooled allocator takes most
  of that back.
- **`std::unordered_map`**, unordered and for reference only, stays faster
  on lookups and random inserts, as expected of a hash map.

## Integer keys

`u64` keys and values, nanoseconds per operation (iterate: per entry);
smaller is better. Bold marks the fastest ordered map of each row.

| cell | bbtree++ | absl btree | std::map | std::unordered_map |
|---|--:|--:|--:|--:|
| u64_u64_random_insert_1k | 28.8 | **20.5** | 22.5 | 11.4 |
| u64_u64_sequential_insert_1k | **7.11** | 19.2 | 27.8 | 9.74 |
| u64_u64_get_hit_1k | **16.0** | 23.2 | 23.6 | 7.68 |
| u64_u64_get_miss_1k | **14.9** | 22.1 | 23.3 | 10.3 |
| u64_u64_iterate_1k | **0.211** | 0.877 | 3.02 | 0.869 |
| u64_u64_range64_1k | 99.1 | **88.1** | 252 | — |
| u64_u64_random_remove_1k | **38.1** | 38.4 | 52.0 | 11.8 |
| u64_u64_entry_upsert_1k | **28.8** | 33.6 | 38.3 | 15.7 |
| u64_u64_pop_first_1k | 14.2 | **7.91** | 8.83 | — |
| u64_u64_bulk_build_1k | **2.82** | 4.72 | 31.7 | 10.1 |
| u64_u64_random_insert_1M | **86.5** | 93.3 | 199 | 58.0 |
| u64_u64_sequential_insert_1M | **10.5** | 36.5 | 79.5 | 56.0 |
| u64_u64_get_hit_1M | **90.8** | 101 | 207 | 25.9 |
| u64_u64_get_miss_1M | **63.5** | 104 | 190 | 27.3 |
| u64_u64_iterate_1M | **0.785** | 1.68 | 52.2 | 17.2 |
| u64_u64_range64_1M | 234 | **222** | 3152 | — |
| u64_u64_random_remove_1M | 92.4 | **92.2** | 276 | 82.7 |
| u64_u64_entry_upsert_1M | **71.5** | 86.4 | 198 | 45.9 |
| u64_u64_pop_first_1M | 16.8 | **8.26** | 43.0 | — |
| u64_u64_bulk_build_1M | **5.19** | 7.18 | 87.4 | 58.1 |
| u64_u64_random_insert_10M | 248 | **237** | 687 | 199 |
| u64_u64_get_hit_10M | **245** | 318 | 468 | 37.1 |
| u64_u64_get_miss_10M | **172** | 310 | 471 | 42.7 |

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
run, the other maps' from the main run (Setup). Bold marks the fastest
ordered map of each row.

| cell | bbtree++ | compact string | absl btree | std::map | std::unordered_map |
|---|--:|---|--:|--:|--:|
| str-16_u64_random_insert_1k | **38.9** | `compact_string<2>` | 107 | 108 | 22.3 |
| str-16_u64_sequential_insert_1k | **14.9** | `compact_string` | 50.0 | 121 | 26.4 |
| str-16_u64_get_hit_1k | **32.5** | `compact_string_24` | 72.5 | 82.9 | 20.4 |
| str-16_u64_get_miss_1k | **23.0** | `compact_string_24` | 77.5 | 76.4 | 21.5 |
| str-16_u64_iterate_1k | **0.418** | `compact_string` | 1.22 | 3.58 | 2.16 |
| str-16_u64_range64_1k | **120** | `compact_string` | 159 | 325 | — |
| str-16_u64_random_remove_1k | **62.7** | `compact_string` | 109 | 110 | 24.8 |
| str-16_u64_entry_upsert_1k | **41.9** | `compact_string` | 115 | 105 | 32.0 |
| str-16_u64_pop_first_1k | **14.5** | `compact_string` | 29.4 | 15.5 | — |
| str-16_u64_bulk_build_1k | **8.99** | `compact_string` | 18.5 | 129 | 22.9 |
| str-json_struct32_build_many_1Mx4 | 61.9 | `compact_string<2>` | **46.3** | 51.5 | 42.9 |
| str-json_struct32_get_many_1Mx4 | 75.9 | `compact_string_24` | **73.8** | 100 | 55.9 |
| str-json_struct32_iterate_1Mx4 | **3.31** | `compact_string<2>` | 3.46 | 4.51 | 4.40 |
| str-json_struct32_build_many_100kx32 | **53.0** | `compact_string<2>` | 101 | 86.9 | 52.0 |
| str-json_struct32_get_many_100kx32 | **53.8** | `compact_string<2>` | 66.6 | 85.6 | 48.2 |
| str-json_struct32_iterate_100kx32 | **5.20** | `compact_string<2>` | 16.6 | 25.4 | 20.9 |
| str-json_u64_random_insert_1M | **133** | `compact_string<2>` | 352 | 457 | 161 |
| str-json_u64_get_hit_1M | **194** | `compact_string_24` | 394 | 638 | 142 |
| str-json_u64_get_miss_1M | **142** | `compact_string<2>` | 422 | 618 | 109 |

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
| str-16_u64_random_insert_1k | 58.8 | 39.7 | **38.9** | 41.7 |
| str-16_u64_sequential_insert_1k | 29.8 | **14.9** | 16.2 | 17.2 |
| str-16_u64_get_hit_1k | 37.8 | 38.2 | 38.1 | **32.5** |
| str-16_u64_get_miss_1k | 23.4 | 24.6 | 26.3 | **23.0** |
| str-16_u64_iterate_1k | 0.432 | **0.418** | 0.442 | 0.485 |
| str-16_u64_range64_1k | 150 | **120** | 126 | 127 |
| str-16_u64_random_remove_1k | 112 | **62.7** | 64.5 | 64.5 |
| str-16_u64_entry_upsert_1k | 71.5 | **41.9** | 43.8 | 42.5 |
| str-16_u64_pop_first_1k | 73.6 | **14.5** | 15.6 | 17.8 |
| str-16_u64_bulk_build_1k | 19.2 | **8.99** | 10.7 | 10.7 |
| str-json_struct32_build_many_1Mx4 | 73.3 | 64.3 | **61.9** | 64.5 |
| str-json_struct32_get_many_1Mx4 | **74.4** | 77.0 | 76.2 | 75.9 |
| str-json_struct32_iterate_1Mx4 | 5.40 | 3.63 | **3.31** | 5.33 |
| str-json_struct32_build_many_100kx32 | 94.6 | 54.7 | **53.0** | 58.8 |
| str-json_struct32_get_many_100kx32 | 62.3 | 55.3 | **53.8** | 60.7 |
| str-json_struct32_iterate_100kx32 | 7.23 | 5.45 | **5.20** | 6.25 |
| str-json_u64_random_insert_1M | 226 | 138 | **133** | 172 |
| str-json_u64_get_hit_1M | 228 | 213 | 211 | **194** |
| str-json_u64_get_miss_1M | 159 | 144 | **142** | 154 |

- **Writes:** a string → `u64` leaf entry shrinks from 49 bytes to 33 and
  shifts by one `memmove`: 1M JSON-key inserts 226 → 138 ns; removes,
  upserts, bulk builds and builds of 32-entry maps 0.45-0.6×; `pop_first`
  0.2×.
- **Iteration:** 0.67× `std::string`'s for maps of 4, 0.75× for maps of
  32. A first layout with 15 bytes in place decoded the size in eight
  instructions and walked 1.2-1.4× slower; PORTING.md, [compact_string](../../PORTING.md#p29-compact_string-12-bytes-in-place-the-size-in-one-place),
  has the measurement.
- **Lookups** with `compact_string` are within 5% of `std::string`'s or
  faster: with 12 bytes in place, 16% of JSON keys and most `str-16` keys
  live on the heap, so a tie reads a line past the node, and `data()`
  branches on the form. Not profiled further.
- **A 2-byte size** buys 2 bytes in place: 90% of JSON keys against 84%.
  Misses in 1M maps came out from 9% faster to 11% slower across runs,
  and the rest within run-to-run noise: no gain outside noise. Strings are capped at
  65,535 bytes.
- **24 bytes** hold most `str-16` keys (median 16) in place: hits 15%
  faster at 1k and 9% at 1M JSON keys. A string → `u64` leaf entry is 41
  bytes, as the Rust crate's `String`, against 33: inserts 5-25% slower,
  walks of many maps 15-47%, `pop_first` 23%.
- `compact_string` stays the 16-byte default: no cap, and nothing the
  2-byte size wins is outside noise.

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

## On glibc's allocator

Every other table here runs on mimalloc, as the Rust bench does.
`bbtree-bench-system` runs the same cells on the system's allocator, glibc
2.39 here, whose aligned `operator new` places bbtree++'s 1 KiB pages
with gaps between them and on 4 KiB pages: walks then take about 3× as
long, and bulk builds over 4.5×. The opt-in `pooled_allocator`
([packed pages](../../PORTING.md#p32-pooled_allocator-pages-packed-into-chunks-opt-in))
takes most of that back. Two runs of each binary at load under 0.3, the
range of the two:

| cell | mimalloc | glibc | glibc, pooled |
|---|--:|--:|--:|
| u64_u64_iterate_1M | 0.67-0.81 | 2.30-2.47 | 0.66-0.78 |
| u64_u64_bulk_build_1M | 5.15-5.24 | 24.5-24.6 | 8.15-8.29 |
| u64_u64_random_insert_1M | 85.6-109 | 137-145 | 88.7-92.4 |
| str-json_u64_random_insert_1M | 234-245 | 351-378 | 236-240 |
| str-json_u64_get_hit_1M | 232-239 | 266-267 | 255-261 |

- **On mimalloc** the pool runs the same instructions as the global
  allocator, and its cells fell on either side of the global allocator's
  from one window of runs to the next. Only bulk builds were slower in
  every pair, by 6-18% on this build, the extra time spent outside user
  code, where the pool's fresh chunks are faulted in. It stays opt-in for
  that, and for what it costs small maps.
- **`u64` lookups** at 1M are left out: `u64_u64_get_hit_1M` moved between
  82 and 132 ns from run to run on either allocator, with or without the
  pool.
- **Walks** at 1M moved too: `u64_u64_iterate_1M` took 2.3-3.3 ns an entry
  on glibc across four runs, and 0.66-2.2 with the pool, which walks as fast
  as on mimalloc in most runs but not every one.

## Against abseil's B-tree

String figures are bbtree++'s with its best compact string.

- **bbtree++ is faster:**
  - lookups: 1.1-1.8× on u64, 2-3.4× on 16-byte strings, 2-3× on 1M JSON
    strings;
  - string writes (inserts, upserts, removes, bulk builds, `pop_first`):
    1.7-2.75×;
  - sequential inserts: 2.7-3.5×;
  - iterating: 3-4× at 1k, 2.1× at 1M, 3.2× for 100k maps of 32;
  - u64 upserts and bulk builds: 1.2-1.7×;
  - `range64` on strings: 1.3×.
- **abseil is faster:**
  - random u64 inserts at 1k: 1.4×;
  - `range64` on u64: 1.05-1.15×;
  - `pop_first` on u64: 1.8-2×;
  - building many 4-entry maps: 1.3×.
- **About even:** u64 removes, random u64 inserts at 1M and 10M (bbtree++
  8% faster at 1M, abseil 5% at 10M), lookups in 4-entry maps, and
  iterating them (3.31 ns per entry against 3.46).

The Rust original has the 1k insert gap too: COMPARISON.md has
bbtree (Rust) at 35.6 ns on 1k random u64 inserts against `BTreeMap`'s
17.6, so that gap comes from the design, not bbtree++. Building many
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
under 0.3, mimalloc, `std::string` keys for both. Nanoseconds per operation (iterate and many maps: per entry);
smaller is better; bold marks the faster. Rows differing by more than 10%:

| cell | bbtree++ | stx_btree |
|---|--:|--:|
| str-16_u64_entry_upsert_1k | **69.0** | 82.9 |
| str-16_u64_get_miss_1k | **22.3** | 25.5 |
| str-16_u64_iterate_1k | **0.418** | 1.17 |
| str-json_struct32_build_many_1Mx4 | **72.9** | 133 |
| str-json_struct32_get_many_100kx32 | 61.4 | **54.7** |
| str-json_struct32_iterate_100kx32 | **7.41** | 10.1 |
| str-json_struct32_iterate_1Mx4 | **5.31** | 13.3 |
| u64_u64_entry_upsert_1M | **69.9** | 83.0 |
| u64_u64_get_hit_1k | 15.3 | **13.1** |
| u64_u64_get_miss_1M | 66.4 | **57.2** |
| u64_u64_get_miss_1k | 14.1 | **12.1** |
| u64_u64_iterate_1M | **0.743** | 1.48 |
| u64_u64_iterate_1k | **0.218** | 1.07 |
| u64_u64_pop_first_1M | **16.8** | 20.1 |
| u64_u64_random_remove_1M | **91.1** | 101 |
| u64_u64_range64_1M | 257 | **231** |

The rest of the 39 cells are within 10%. The two ports' algorithms match
nearly line for line; where stx_btree leads:

- **Lookups (1.15-1.2×), at 1k and 1M misses:** a standalone probe of both maps' `u64` lookups gives
  13.3 against 12.9 ns for hits and 12.8 against 13.0 for misses, 16 more
  instructions a hit in bbtree++; the bench's 1k cells show 15-25% across
  runs, the bench's build of the lookup (not its algorithm) the likelier
  cause. A vectorized count of the leaf's last slots (55 two-byte slots
  leave 7 for a scalar tail) cut the count's instructions by a third and
  changed nothing: lookups wait on their loads, not on the count.
- **`range64` at 1M (1.1×):** within the cell's run-to-run range;
  bbtree++'s runs of it took 232-309 ns.

Sequential inserts were 1.25-1.3× slower until appends checked the shared
prefix out of line (PORTING.md, [loops that store into a page](../../PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)); now
1.06-1.09×.
`range64` was 1.25-1.8× slower before bbtree++'s steps were inlined whole
(PORTING.md, [forward steps](../../PORTING.md#p38-a-step-is-inlined-whole-so-the-position-stays-in-registers)): stx_btree's scan loop keeps its position in
registers, bbtree++'s kept it in memory. Now they are even at 1k.

What bbtree++ took from the comparison (PORTING.md): [packed pages](../../PORTING.md#p32-pooled_allocator-pages-packed-into-chunks-opt-in)
(`pooled_allocator`), [install rules](../../PORTING.md#p34-installing-find_packagebbtree-with-the-targets-own-names) checked by a moved install,
[short windows](../../PORTING.md#p35-a-short-byte-strings-window-overlapping-loads-not-a-copy) by overlapping loads, the shared scan's
[check of the levels it keeps](../../PORTING.md#p36-a-shared-scan-checks-the-levels-it-keeps-before-taking-the-next-leaf), and [unwritten scratch arrays](../../PORTING.md#p37-the-shared-maps-scratch-arrays-stay-unwritten-until-used)
in the shared map.

## Setup

- **Machine and build:** as in COMPARISON.md (Ryzen 9 5950X, Clang 15,
  `--preset bench`, mimalloc 3.3.2 with huge pages), abseil 20250814.2;
  measured on 2026-11-08, in the same window as COMPARISON.md.
- **Every map, every size:** `bbtree-bench small tiny cpp --key
  u64,str-16,str-json` on bbtree++, abseil's B-tree, `std::map` and
  `std::unordered_map`, one run each in a fresh process, 95 s, load under
  0.3 at the start, no compaction stall in the kernel. `cpp` is the C++
  bench's own suite, which runs `ci`'s 1M and 10M workloads on every map
  (`ci` itself runs only bbtree++ and `std::map`, as in the Rust bench).
- **String key types:** `bbtree-bench cpp --key str-16,str-json --size
  1k,1M,1Mx4,100kx32` on bbtree++'s maps, one run each, 23 s.
- **Memory:** `bbtree-bench-memory cpp`, as its section says; the
  counts were the same byte for byte on a rerun after the second round.
