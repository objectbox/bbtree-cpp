# How bbtree++'s figures got where they are

What each round of optimization changed in the comparison with the Rust
original, oldest first. Every table is its round's own measurement, in one
window of its time, on the machine and build that
[COMPARISON.md](COMPARISON.md), which has today's figures, names in its
Setup; figures of different rounds don't compare.

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

## What the review round changed

A review of the port (REVIEW_PLAN.md) and what followed it: in-node counts
in explicit vectors (PORTING.md, [in-node counts](../../PORTING.md#p9-in-node-counts-the-compilers-sse2-masks-and-no-avx-512-or-neon)), lookups that return in
registers and iterators made in place ([iterators](../../PORTING.md#p47-iterators-after-the-review)), the remembered
last leaf kept across inserts that fit, bulk builds' appends, and the
shared map's descent inlined ([the shared descent](../../PORTING.md#p48-the-shared-maps-descent-is-inlined-into-each-operation)). The tree before
and after, in one window, one run each with the bench's `--perf`, which
counts the timed regions' instructions.

| cell | before, ns | after, ns | instructions |
|---|--:|--:|--:|
| u64_u64_get_hit_1k | 16.0 | 9.30 | 252 → 153 |
| u64_u64_get_miss_1k | 15.2 | 7.98 | 229 → 138 |
| u64_u64_random_insert_1k | 29.9 | 23.8 | 406 → 320 |
| u64_u64_random_remove_1k | 40.0 | 34.7 | 413 → 322 |
| u64_u64_entry_upsert_1k | 29.1 | 23.8 | 301 → 214 |
| u64_u64_range64_1k | 96.9 | 85.0 | 1608 → 1489 |
| u64_u64_bulk_build_1k | 2.85 | 2.09 | 57 → 37 |
| u64_u64_get_hit_1M | 90.5 | 74.1 | 402 → 260 |
| u64_u64_get_miss_1M | 71.5 | 50.3 | 378 → 245 |
| u64_u64_random_insert_1M | 90.9 | 82.3 | 565 → 437 |
| u64_u64_entry_upsert_1M | 75.1 | 62.7 | 460 → 329 |
| u64_u64_bulk_build_1M | 5.50 | 4.65 | 66 → 46 |
| str-16_u64_get_hit_1k | 37.1 | 34.1 | 339 → 310 |
| str-json_u64_get_hit_1M | 241 | 231 | 516 → 471 |

The bench itself changed too: it no longer times the destruction of a
build's moved-from input (1.6-5.8% of `build_many`), and `--perf` counts
on CPUs without an L1d miss counter. Under g++ 13 at `-O2 -march=native`,
which none of these tables is built with, the counts' change is larger: a
1k `u64` hit 59 → 11.5 ns, as g++ did not vectorize the loop the counts
were. Memory is unchanged, byte for byte, over 48 cells of
`bbtree-bench-memory`.
