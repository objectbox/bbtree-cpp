# bbtree++ against the Rust original

The port's bench (`bbtree/bench`) against the Rust bench it ports, cell
for cell, on one machine, after the port's first optimization round.

- **Machine:** AMD Ryzen 9 5950X (Zen 3, AVX2, no AVX-512), Linux 6.8,
  quiet (load under 0.3 at the start of each run).
- **Rust:** the original at `3ab503d`, rustc 1.99.0, its bench as shipped
  (mimalloc crate 0.1, which builds mimalloc 3).
- **C++:** Clang 15, `--preset bench` (`-O3 -march=native`, LTO), mimalloc
  3.3.2 (PORTING.md, [the bench's allocator](../../PORTING.md#p17-the-benchs-allocator-mimalloc-332-behind-operator-new)).
- **Inputs:** the same keys, values, orders and probes: `bbtree-bench
  self-check` pins the port's generators to the Rust bench's (PORTING.md,
  [bench inputs](../../PORTING.md#p19-bench-inputs-the-rust-benchs-key-for-key)).
- **Runs:** one per cell, each in a fresh process (AGENTS.md: no repeats to
  average noise away). Cells whose Rust runs varied by more than 10% across
  this session are marked.
- **Reproduce:** `bbtree-bench ci --out A` in each bench, then
  `bbtree-bench compare --head A/cpp.csv --base A/rust.csv`.

## The `ci` suite: the unsync map, ns an operation (iterate: an entry)

C++ ÷ Rust below 1 is the port faster.

| cell | Rust | C++ | C++ ÷ Rust | Rust `BTreeMap` | C++ `std::map` |
|---|--:|--:|--:|--:|--:|
| u64_u64_random_insert_1k | 36.0 | 29.5 | 0.82 | 17.5 | 22.7 |
| u64_u64_sequential_insert_1k | 8.04 | 7.07 | 0.88 | 23.4 | 28.5 |
| u64_u64_get_hit_1k | 24.8 | 16.0 | 0.64 | 29.5 | 23.6 |
| u64_u64_get_miss_1k | 23.2 | 14.6 | 0.63 | 29.3 | 22.5 |
| u64_u64_iterate_1k | 0.318 | 0.267 | 0.84 | 1.04 | 3.06 |
| u64_u64_range64_1k | 119 | 113 | 0.95 | 106 | 253 |
| u64_u64_random_remove_1k | 44.5 | 38.5 | 0.87 | 47.0 | 50.6 |
| u64_u64_entry_upsert_1k | 46.1 | 29.1 | 0.63 | 37.5 | 37.8 |
| u64_u64_pop_first_1k | 14.8 | 14.6 | 0.98 | 17.2 | 8.87 |
| u64_u64_bulk_build_1k | 3.92 | 4.96 | **1.27** | 2.39 | 31.1 |
| u64_u64_random_insert_1M | 92.2 | 82.9 | 0.90 | 102 | 224 |
| u64_u64_sequential_insert_1M | 10.2 | 10.3 | 1.01 | 46.3 | 79.5 |
| u64_u64_get_hit_1M | 94.0 | 93.4 | 0.99 | 151 | 230 |
| u64_u64_get_miss_1M | 83.3 | 62.4 | 0.75 | 147 | 198 |
| u64_u64_iterate_1M | 0.729 | 0.632 | 0.87 | 3.26 | 58.8 |
| u64_u64_range64_1M | 272 | 288 | 1.06 | 375 | 3205 |
| u64_u64_random_remove_1M | 105 | 105 | 1.01 | 118 | 271 |
| u64_u64_entry_upsert_1M | 94.7 | 84.3 | 0.89 | 96.1 | 196 |
| u64_u64_pop_first_1M | 18.5 | 16.7 | 0.90 | 19.8 | 40.1 |
| u64_u64_bulk_build_1M | 5.44 | 7.28 | **1.34** | 4.56 | 89.4 |
| u64_u64_random_insert_10M | 243 | 260 | 1.07 | 263 | 725 |
| u64_u64_get_hit_10M † | 255 | 246 | 0.96 | 301 | 484 |
| u64_u64_get_miss_10M | 197 | 188 | 0.95 | 344 | 480 |
| str-16_u64_random_insert_1k | 46.6 | 59.7 | **1.28** | 82.6 | 106 |
| str-16_u64_get_hit_1k | 44.5 | 37.4 | 0.84 | 67.3 | 84.0 |
| str-16_u64_get_miss_1k | 29.8 | 23.0 | 0.77 | 78.7 | 79.4 |
| str-json_u64_random_insert_1M | 182 | 258 | **1.42** | 306 | 454 |
| str-json_u64_get_hit_1M | 278 | 273 | 0.98 | 519 | 670 |
| str-json_u64_get_miss_1M | 210 | 161 | 0.77 | 515 | 694 |

† Rust's runs of this cell ranged from 231 to 255 ns this session.

The two right columns compare different maps: Rust's `BTreeMap` is a B-tree,
`std::map` a red-black tree.

## Many small maps (`tiny`), ns an entry

| cell | Rust | C++ | C++ ÷ Rust |
|---|--:|--:|--:|
| str-json_struct32_build_many_1Mx4 | 61.0 | 80.3 | **1.32** |
| str-json_struct32_get_many_1Mx4 | 82.2 | 74.2 | 0.90 |
| str-json_struct32_iterate_1Mx4 † | 4.52 | 8.27 | **1.83** |
| str-json_struct32_build_many_100kx32 | 60.7 | 103 | **1.69** |
| str-json_struct32_get_many_100kx32 | 67.4 | 70.7 | 1.05 |
| str-json_struct32_iterate_100kx32 | 8.82 | 7.31 | 0.83 |

† Rust's runs of this cell ranged from 4.5 to 6.9 ns this session.

## String keys as `bbtree::compact_string`, ns an operation (iterate: an entry)

The port's `compact_string` (PORTING.md, [compact_string](../../PORTING.md#p29-compact_string-12-bytes-in-place-the-size-in-one-place)) is a C++ addition: a 16-byte
string that holds up to 12 bytes in place and moves as bytes, where
libstdc++'s `std::string` is 32 and the Rust crate's `String` 24. The
`std::string` and Rust columns are the tables' above; the compact column is
`bbtree-bench cpp --map bbtree-unsync-compact` on the same cells, one run
each, started at load 0.3, after the root-leaf walk (PORTING.md, [iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)); the iterate rows
after its fold, in one window with a Rust run that took 5.12 and 8.97.

| cell | Rust `String` | C++ `std::string` | C++ `compact_string` | compact ÷ Rust |
|---|--:|--:|--:|--:|
| str-16_u64_random_insert_1k | 46.6 | 59.7 | 39.1 | 0.84 |
| str-16_u64_get_hit_1k | 44.5 | 37.4 | 39.4 | 0.89 |
| str-16_u64_get_miss_1k | 29.8 | 23.0 | 25.1 | 0.84 |
| str-json_u64_random_insert_1M | 182 | 258 | 161 | 0.88 |
| str-json_u64_get_hit_1M | 278 | 273 | 249 | 0.90 |
| str-json_u64_get_miss_1M | 210 | 161 | 176 | 0.84 |
| str-json_struct32_build_many_1Mx4 ‡ | 61.0 | 80.3 | 66.1 | **1.08** |
| str-json_struct32_get_many_1Mx4 | 82.2 | 74.2 | 79.5 | 0.97 |
| str-json_struct32_iterate_1Mx4 † | 4.52 | 8.27 | 6.30 | **1.39** |
| str-json_struct32_build_many_100kx32 | 60.7 | 103 | 61.0 | 1.00 |
| str-json_struct32_get_many_100kx32 | 67.4 | 70.7 | 68.3 | 1.01 |
| str-json_struct32_iterate_100kx32 | 8.82 | 7.31 | 5.17 | 0.59 |

† Rust's runs of this cell ranged from 4.5 to 6.9 ns this session.
‡ From a later run with no compaction stall in the kernel: in this one the
cell's 1.1 GB of fresh pages stalled it for huge pages (139 ns, and
`std::string` keys 236 in the same conditions; CPP_MAPS.md).

With compact keys the port's string inserts and builds of 32-entry maps
catch up with the Rust crate's or pass it, lookups stay where
`std::string`'s are, and iterating maps of 32 is faster than the Rust
crate's. Iterating maps of 4 still trails, 6.30 ns against 5.12 in the
same window. The walks run about the same instructions an entry (sampled;
PORTING.md, [iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)); a raw loop over the same lines takes 5.0 ns with compact keys, so
the rest is the walk's per-map work, and with `std::string` keys the
64-byte entries' extra lines.

## The shared map, u64 → u64

The benches run the shared map at 1k only; the 8-thread rows come from the
same probe in both languages (1M scrambled keys, 8 threads, wall time an
operation; `bbtree::bbtree_map` against `bbtree::BBTreeMap`).

| u64 → u64, 1k | Rust shared | C++ shared | Rust unsync | C++ unsync |
|---|--:|--:|--:|--:|
| random_insert | 37.8 | 36.9 | 33.9 | 29.3 |
| sequential_insert | 11.6 | 9.51 | 7.89 | 6.86 |
| get_hit | 25.5 | 19.8 | 23.8 | 15.3 |
| get_miss | 23.7 | 17.5 | 22.4 | 14.4 |
| iterate (a scan) | 0.829 | 0.828 | 0.325 | 0.246 |
| range64 | 99.4 | 87.8 | 119 | 110 |
| random_remove | 49.3 | 46.5 | 43.2 | 38.0 |

| 1M, 8 threads | Rust | C++ |
|---|--:|--:|
| insert | 42 | 45 |
| get_copied | 54 | 56 |
| update | 21 | 22 |
| remove | 40 | 42 |

## bbtree-store, `examples/bench` at 1M entries

| | Rust | C++ |
|---|--:|--:|
| 1 thread: insert, ns | 254 | 253 |
| 1 thread: get, ns | 233 | 224 |
| 1 thread: scan, ns an entry | 19.1 | 28.5 |
| nodes + heap, bytes an entry | 103.6 | 110.6 |
| churn, heap bytes a live record byte (round 4) | 1.52 | 1.74 |
| 8 threads: insert, ns wall | 75 | 67 |
| 8 threads: get, ns wall | 39 | 36 |
| 4,000-byte values: insert / get, ns | 1744 / 569 | 1879 / 721 |

The store keeps a head's record in place (the race TSan found; PORTING.md, [heads' records](../../PORTING.md#p24-a-leafs-head-keys-record-stays-put)), which
costs up to 8 more bytes a record and the stubs churn leaves: the bytes
and churn rows.

## What the first optimization round changed

| change | effect |
|---|---|
| huge pages in the bench, as the Rust bench's mimalloc has (PORTING.md, [bench dependencies](../../PORTING.md#p12-bench-dependencies-include-mimalloc), [the allocator](../../PORTING.md#p17-the-benchs-allocator-mimalloc-332-behind-operator-new)) | iteration 2× faster: most of the first measurement's gap |
| `for_each`, a depth-first walk with no cursor ([iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)) | 1M u64 walk 0.94 → 0.56 ns an entry, 1k 0.69 → 0.25, 1M maps of 4: 18 → 8 |
| one move per shifted object, not two through a temporary ([moves](../../PORTING.md#p6-moves-relocate-only-trivially-relocatable-types-as-bytes)) | 1M `std::string` inserts 252 → 221 ns |
| bulk builds: sorted ranges build in place; a leaf's run appends without the prefix shrink | 1M u64 bulk build 8.1 → 7.0 ns in the bench, 13.2 → 9.6 on a probe |
| shared map appends check the last leaf before latching it, as the Rust crate does (a porting omission) | 8-thread inserts 110 → 45 ns; the store's 143 → 67 |

The iterate cells above predate two later changes ([iteration](../../PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)): a root leaf
walked with no call, and a checksum that folds, as the Rust bench's does,
in place of a `for_each` adding into a captured sum. In one window with the
Rust bench, one run each:

| iterate | Rust | C++ |
|---|--:|--:|
| u64_u64_iterate_1k | 0.343 | 0.228 |
| u64_u64_iterate_1M | 0.831 | 0.685 |
| str-json_struct32_iterate_1Mx4 | 5.12 | 7.18 |
| str-json_struct32_iterate_100kx32 | 8.97 | 7.47 |

## Where the port trails, and why

- **`std::string` inserts (1.3-1.4×), small string maps' builds
  (1.3-1.7×):** libstdc++'s `std::string` is 32 bytes against `String`'s
  24 (19 entries a `std::string` → `u64` leaf against 24: more leaves and
  misses), and it points into itself while short, so it shifts one object
  at a time where Rust moves bytes (PORTING.md, [moves](../../PORTING.md#p6-moves-relocate-only-trivially-relocatable-types-as-bytes)). Moving strings as bytes with a
  pointer fix-up made 100k maps of 32 strings 26% faster to build in a
  prototype, at the cost of copying the bytes of a non-trivially-copyable
  type and depending on libstdc++'s layout: FUTURE_WORK.md.
  `bbtree::compact_string` keys, 16 bytes and moved as bytes, close the
  insert and 32-entry build gaps (the section above).
- **Bulk builds (1.3×):** a probe of the same build (sorted random u64,
  huge pages) takes 5.3 ns an entry, close to the Rust bench's 5.2; the
  bench cell's 7.0 ns stays unexplained.
- **Iterating 1M maps of 4 entries (1.8×, with Rust's variance):** each map
  is a cold root page; the walk itself is down to a direct loop's cost.
- **The store's scan (1.5×) and long values (1.1-1.3×):** a scan's time
  is in reading each entry's record in the heap pages, a cache miss an
  entry, as in the Rust design; the port's heap takes 7 more bytes an entry
  (87 against 80). Where the bytes and the misses differ is not pinned
  down.

Everything else is within 10% or faster, mostly from the port's lookups
(`get_hit`/`get_miss` 0.63-0.99×) and the upsert path.
