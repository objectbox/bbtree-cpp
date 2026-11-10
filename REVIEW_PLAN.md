# Review plan

A second look at the finished port, on 2026-11-09: the code, for defects and
for performance it leaves unused, and the porting decisions (PORTING.md) and
process, for paths that look sub-optimal in hindsight. Each item is a
hypothesis with the evidence the review found, the experiment that settles
it, and, once run, the outcome.

Seven reviews ran in parallel, one per area (node layer, unsync tree and API,
shared map, store and WAL, file maps with allocators and compact strings,
test and API parity with the Rust crate, bench methodology), each against the
Rust original and against what PORTING.md, FUTURE_WORK.md and the bench docs
already record as measured or rejected. Their reports are kept outside the repository (the maintainer's local
branch `review-plan`). The review phase took no timings (seven agents loaded the
machine): its figures are instruction, branch-miss, cache and fault counts.
Timings come from the work packages below.

Status marks: [ ] open, [x] running, [x] done; a done item says what was
kept.

## How the experiments run

- A package works in a worktree and branch of its own (`exp/<name>`), never
  on `main`. A change that is kept is one commit there; a change measured and
  not kept is recorded here with its figures.
- Reason, then count, then time (AGENTS.md, Performance reasoning). Counts
  (`perf stat -e instructions:u,cycles:u,branch-misses:u`, cache and TLB
  events where the claim is about memory) don't depend on load and screen
  every change. Time confirms what passed: base and variant in one quiet
  window, interleaved, two rounds; a gap inside the bands AGENTS.md names
  (10-30% on 1M cells, 5-19% from code placement) with no count that explains
  it is not a result.
- One machine: builds, tests, counts and timings of every package go through
  one lock, and a timing waits for a 1-minute load under 0.3 and reports the
  load other processes put on the machine while it ran.
- Selected cells only, each run under five minutes, no sweep.

## Part 1: decisions and process

### R1. In-node counts were left to the auto-vectorizer

PORTING.md, [in-node counts](PORTING.md#p9-in-node-counts-the-compilers-sse2-masks-and-no-avx-512-or-neon): "a plain loop over a compile-time capacity,
which Clang and GCC vectorize". That holds at `-O3`. Two reviews found
independently that g++ 13 at `-O2` refuses ("Loop costings not worthwhile",
its very-cheap cost model) and compiles the count to 49 or 55 scalar rounds
with a data-dependent branch each:

| build | instructions a `u64` lookup, 1k / 1M | branch misses a lookup |
|---|--:|--:|
| Clang 15 `-O3 -march=native` (every published figure) | 260 / 410 | 0.04 / 0.36 |
| g++ 13 `-O3 -march=native` | 254 / 402 | 0.04 / 0.32 |
| Clang 15 `-O2`, baseline x86-64 (SSE2 masks) | 269 / 476 | 0.04 / 0.36 |
| g++ 13 `-O2`, baseline x86-64 (SSE2 masks, out of line) | 358 / 651 | 0.03 / 0.30 |
| g++ 13 `-O2 -march=x86-64-v2`, `-v3` or `native` | 977 / 1756 | 3.15 / 8.24 |

The last row is what `-O2 -march=native`, RHEL 9's and 10's default flags and
this repository's `gcc` preset with the native test target build. String
keys take the scalar loop under g++ `-O2` at any `-march` (4- and 8-byte
slots have no SSE2 mask path). No number for GCC, for `-O2` or for a baseline
target is in the docs but four walk cells.

Beside the cliff, explicit AVX2 counts beat what Clang's vectorizer makes of
the loop even in the published build: prototypes cut a count of 55 two-byte
slots from 107 instructions to 41 and a `u64` hit from 225 to 161 (1k) and
374 to 276 (1M); a leaf equality mask (the design's D41, on AVX2) cut
`compact_string` lookups by 11-12%. CPP_MAPS.md records that vectorizing the
count's scalar tail "changed nothing" in time, so the time is open; and at
leaf capacity 31 (`compact_string` → `u64`) the tail is 46-50 of a lookup's
instructions, which that try (on `u64`) did not see.

- [x] **WP1** `exp/explicit-count`: counts and masks in explicit SSE2 and
  AVX2, so that no build depends on the vectorizer; the survey of eight
  builds again; time in the published build, in g++ `-O2 -march=native` and
  in g++ `-O2` baseline.

### R2. The default allocator on glibc

PORTING.md, [allocators](PORTING.md#p3-allocators-a-concept-of-page-allocators-not-stds-allocator) and [pooled pages](PORTING.md#p32-pooled_allocator-pages-packed-into-chunks-opt-in): every page is one aligned
`operator new`, 1 KiB aligned to 1 KiB for a leaf, and the pool that takes
back glibc's 1.3-5× is opt-in. The bench runs on mimalloc, so no published
figure is what a default user gets. What the review counted on glibc 2.39:

- A 1 KiB page aligned to 1 KiB costs 2 KiB of heap (the chunk header sits
  before the pointer, so the next aligned address is 2 KiB on), structurally.
- The loss in lookups and walks is the 4 KiB OS pages, not the gaps: packed
  pages on 4 KiB pages still miss the dTLB 1.87 times a hit against 2.02.
- `GLIBC_TUNABLES=glibc.malloc.hugetlb=1` gives the default allocator the
  pool's counters with no code (dTLB misses a hit 2.02 → 0.01, build faults
  15,176 → 1,523). It is in no doc.
- Nodes aligned to 64 bytes instead of their size (only bit 0 of a node
  pointer is a tag; D15 chose 1 KiB so that a node "never crosses a 4 KB
  page") took a 1M map's peak RSS from 63 MB to 39 MB and its build's faults
  down 40%, with no counter worse; the design's cost model rejected it on
  mimalloc evidence only.
- The pool's threads sleep on its mutex (15k voluntary context switches per
  200k page requests at 8 threads; the zeroing and the 2 MiB fault run under
  it), which the CPU profile that cleared the mutex could not see; and it
  maps 4 MiB and trims for every 2 MiB chunk.

- [x] **WP2** `exp/glibc-default`: time the tunable (with `std::map` and
  abseil on glibc beside it, which no doc has); 64-byte alignment on glibc
  and on mimalloc; the pool's chunks from aligned `operator new` on the cell
  that would refute a pooled default (1M bulk build on mimalloc); the pool's
  lock.
- [x] **WP9**, not built (WP2's figures leave no case for it): a default that needs no opt-in: the first pages
  from `operator new`, then chunks the map owns (the design is in
  the file maps' and allocators' review); only if WP2's figures leave a case for it.

### R3. Comparisons built unequally, and what the bench times

- The store table and the 8-thread table of COMPARISON.md compare a C++
  build for the host CPU with LTO against Rust binaries that every trace
  found says were built for baseline x86-64 without LTO (cargo fingerprints
  with empty `rustflags`, no AVX instruction in them; only the bench crate
  has `target-cpu=native` and fat LTO). The gaps published there, 3-14%, are
  of the size the flags can make.
- String rows give bbtree++ the best of three compact strings a cell and
  the other maps their standard string; `iterate` rows fold in bbtree++ and
  step iterators elsewhere. Like for like, bbtree++ is slower than the Rust
  crate on 5 of 12 string rows and its memory is 0.74-1.08× `std::map`'s.
- The C++ bench times the destruction of the moved-from input of string
  builds, which the Rust bench doesn't (against bbtree++, an estimated 4-6%
  of `build_many`).
- One sample a cell, no pinning, timed totals from 0.2 ms: the variance is
  made at the source.

- [x] **WP3** `exp/fair-comparison`: the store bench and the 8-thread probe
  against Rust built as the bench crate builds; the input's destruction out
  of the timed region; whether pinning the cell's child and fixing its
  address space narrow a cell's run-to-run range; `--perf` counting what the
  CPU has instead of refusing.

### R4. The tests were ported thin, and Shuttle became stress tests

PLAN.md still has "port `unsync/map/tests`, `unsync/set/tests`" unchecked;
PORTING.md's scope says the tests are ported. By count: the shared map has 17
cases for the Rust crate's 72, the node level 21 for 118, file maps 4 for 20.
The parity review ported the highest-risk groups as standalone programs and
ran them under ASan and UBSan: drop accounting (80 scenarios), allocator
refusal at every allocation point (44), std::map expectations, hostile file
maps. They found eleven defects (Part 2), six of which ordinary code can hit.
So the decision cost correctness, and is the best-evidenced item here.

Also: `BBTREE_CHECKED` gates nothing (no header reads it); the `asan`, `tsan`
and `fuzz` presets build `RelWithDebInfo`, so CI's sanitizer and fuzz runs
have assertions off; the fuzz harnesses drive `u64` values only and were not
touched through 35 later library commits; the TSan stress tests merge a leaf
0 or 1 times a run.

- [x] **WP6a** `exp/tests-unsync`: the ported unsync tests into `bbtree/tests`
  with the fixes they need, one commit a defect.
- [x] **WP6b** `exp/tests-file-maps`: the same for file maps and bbtree-mmap,
  with a fuzz target for checked opens.
- [x] **WP7a** `exp/shared-map-tests`: the shared map's missing tests, and
  switch points under a test macro to reach the interleavings Shuttle
  explored. Seven of its tests have no C++ test that can reach theirs; a
  prototype of 24 switch points with a model-checked stress found the scan
  defect of Part 2 unaided and caught 5 of 7 removed-check mutants.

### R5. The racing-reads checker was dropped

PORTING.md, [no checker](PORTING.md#p15-no-racing-reads-checker-on-the-compiled-code), argues that an acquire fence and a relaxed reload
order the racing reads. The reads are plain loads, so the compiler may assume
the memory doesn't change and reload a value after the check: what holds is
practical, that Clang and GCC treat the fence as clobbering memory.

- **Clean today.** The Rust crate's checker, adapted to Clang's and GCC's
  output with a 12-line change (so the cost the decision names was small),
  found no read of a node after its check that reaches a result in eight
  builds (Clang 15 and GCC 13, `-O2` and `-O3`, with and without
  `-march=native`), for `get_copied`, `contains`, `read`, the descent, three
  scans and `insert`; it flags two planted reloads, but for one instance at
  GCC `-O2`. AArch64 was not audited.
- **To keep it clean:** an empty `asm volatile` that clobbers memory in
  `is_unchanged`, at no instruction on either compiler, and the checker with
  its planted self-test in CI.

- [x] **WP7a** `exp/shared-map-tests`, with R4's shared-map tests.

### R6. bbtree-store: a head's record stays put

PORTING.md, [heads' records](PORTING.md#p24-a-leafs-head-keys-record-stays-put): 7 bytes an entry and more churn, on the claim that the
Rust crate has the same race, which had never been reproduced there.

- **The decision stands: the race is the Rust crate's.** ThreadSanitizer
  reports it on the unmodified Rust sources with one test file added, 33 to 44
  reports in three of three runs: `tree/prefix.rs:131` and
  `node/internal.rs:496` read a head's key while `leaf_heap.rs:458` moves its
  record under the leaf's latch alone. Its CI's TSan job runs `map::tests`
  only. Worth reporting upstream, with the test file.
- **The cost is not where the docs put it.** With heads free to move (one
  thread) the port takes the Rust crate's 103.6 bytes an entry and 1.52 churn
  exactly: all 7 bytes come from pages a head's record pins, none from the
  8 bytes of room or the stubs, as COMPARISON.md and PORTING.md say. Pinning
  also makes 45-59% more compactions, 40% of the insert phase's futile.
- **Proposed, not built:** move a head's record under a try-latch of its
  separator's node, which the descent can hand the closure in two words.

### R7. bbtree-store: an insert refused by an exception

PORTING.md, [refused inserts](PORTING.md#p21-refusing-an-insert-by-throwing-not-by-a-result-type); FUTURE_WORK.md has it as unmeasured. Counted: a
cache that evicts and retries at its budget sees about one refusal an insert
(99.4% of inserts of 2,000-byte values refused once; bursts of up to 2,486 in
a row for 32-byte values), and a refusal is 21,491 instructions by exception
against 2,501 returned as a value (a prototype: closures returning
`std::optional`, an early return after the three `policy.vacant` calls).

- [x] **WP8** `exp/store`: the refusal as a value, timed on a store at its
  budget; a head's record moved under its separator's node's try-latch,
  measured in bytes, churn and 8-thread inserts; the overflow pages that leak
  on `std::bad_alloc`; CRC32C, which every `read_value` and every open of a
  WAL pays, not one commit. The gets of long values run the Rust crate's
  instructions (1,324 against 1,333); what the bench row loses is 8 of 66
  lines a get that the prefetcher doesn't fetch in that binary, unexplained.

### R8. The Rust crate's always-on checks became `assert`

The file maps' reader in Rust relies on `assert!` and checked arithmetic that
are on in release builds; ported as `assert` and plain `+`, they are off in
release. A checked `open` therefore does not make reads memory-safe (three
holes reproduced under ASan; Part 2). The pattern is a porting hazard beyond
file maps: every Rust `assert!`, slice index and checked operation that
guards against input rather than against the crate's own bugs.

- [x] **WP6b**: the file maps' holes; and a list of the remaining `assert`s
  that guard input.

## Part 2: code findings

### Defects, all reproduced unless marked

| where | what | found by |
|---|---|---|
| `detail/tree/remove.hpp:252` | a nested braced `std::pair` copies the removed entry: move-only keys or values can't `erase`, `pop_first`, `remove`, `retain` (compile error); copyable ones are copied about 1 removal in 10-20, inside `noexcept` | tree, node, parity |
| `unsync/map.hpp:483-500` | `erase(it, it)` on a non-end iterator erases to the end and past it | tree, parity |
| `detail/tree/tree_owner.hpp:67-94` | a range insert into a map emptied by `erase` (or after a first insert that threw) leaks its root page | tree |
| `detail/tree/insert.hpp:67-104` | a mapped type that can't be assigned can't be inserted at all: `assign` is a run-time flag | tree |
| `detail/tree/tree_owner.hpp:98-134` | a page refused during `append` leaves both maps with moved-from keys and null values at unchanged sizes, against PORTING.md's "the map is then as it was" | tree, parity |
| `detail/tree/insert.hpp:149-150` | GCC only: a key copy that throws during a split leaks the reserved leaf page (argument evaluation order) | tree, parity |
| `detail/tree/cursor.hpp:258` | iterators hold the map object's address: after `swap` they walk the other map, after a move they crash; undocumented | tree |
| `detail/node/keys.hpp:54-57` | keys whose `<=>` is a `partial_ordering` (`double`, `float`, pairs of them) don't compile | node |
| `unsync/map.hpp:396`, `set.hpp:287` | a range of elements that aren't `K` under `std::less<>` (C strings into a `std::string` map) can build an unsorted map | parity |
| `unsync/set.hpp:426` | `a & b` builds on the right set's allocator when the right set is 16× smaller | parity |
| `file.hpp:285` | a checked open: `offset + page.size` wraps, and the check itself reads before the buffer | periphery, parity |
| `file.hpp:267-324` | a checked open accepts an empty leaf below the root (iteration then runs off the page) and a leaf without its head flag (separators read head slots the check skipped; for `var_bytes` a pointer the file supplies) | periphery |
| `file.hpp:207-244` | the fingerprint leaves out the storage format: a `u64 → u64` file opens as `u64 → std::string` | periphery, parity |
| `file.hpp:477-496` | the writer copies stale edges and slots: heap pointers in the file, and equal maps give different files | periphery, parity |
| `file.hpp:49-51` | `plain_bytes` admits `bool`, pointers, `std::string_view`, `long double` (by reading) | periphery |
| `bbtree_mmap/heap_pages.hpp:86-90` | `bbtree_map_file<uint32_t, std::string>` writes files it then refuses as corrupt | parity |
| `pooled_allocator.hpp` | a moved-from `pooled_allocator` has a null pool; using it crashes | periphery |
| `map.hpp` (shared) | `get_or_insert` with a void closure and `extend` with move-only entries don't compile; `update_or_insert`, `get_or_insert`, `remove_if`, `get_owned` have no call site in any test | parity |
| `bbtree_store/store.hpp:139-159` | `insert` catches `alloc_error` only: a `std::bad_alloc` from compaction's vectors leaves the value's overflow pages unowned | store |

| `detail/latched/scan.hpp:128-135`, `:156-165` | `scan_from` and `scan_rev_from` start on the wrong side of a bound outside the shared prefix when a prefix shrink races: the prefix is read unvalidated, where the Rust crate checks again under the border leaf's latch (and has a Shuttle test). 11,346 of 29,738 shrinks hit forward. bbtree-store's visitors filter by range, so it is shielded | shared |
| `detail/latched/latched_tree.hpp:392` | `view()` reads the shared prefix outside the annotated racing reads (TSan reports it in a four-level tree); the copy is unused, so benign today | shared |
| `detail/latched/scan.hpp:175-181` | `descend_to_border` keeps a stale shared latch on the root page across its retry (by reading) | shared |

The shared map's latch word, descent, splits, removals, shrinks, page reuse
and exception paths, and the store's WAL, budget accounting, stubs,
generations and scans against writers, were checked and found sound.

Deviations from std::map that README.md doesn't list: references and
pointers, not only iterators, are invalidated by every insert and erase;
`extract`, `merge`, node handles, `crbegin`, `value_comp`, transparent
`equal_range`, allocator-extended constructors and deduction guides are
missing; `std::erase_if` and `std::views::keys` don't compile; `for (const
std::pair<const K, V>& e : map)` compiles and copies every entry. Docs that
the tree doesn't back: `try_insert` (cited as present), `extract_if` (named
in `unsync/map.hpp`'s header comment), README's "245 tests" (146 distinct
cases), `node_config.hpp`'s example (doesn't compile).

### Performance, by instructions (Clang 15 `-O3 -march=native`)

| where | what | counted | package |
|---|---|---|---|
| `unsync/map.hpp:101-113`, `set.hpp:89-101` | the iterators' `++`/`--` wrappers aren't `always_inline`; Clang outlines them, so a range-for step is a call, and under LTO stays inlined only while the program has one loop a map type | 34.7 → 12.7 an entry | WP4 |
| `rbegin()`/`rend()` | `std::reverse_iterator` copies the 288-byte iterator at every dereference, and `rend()` descends each round | 389 an entry against 16 for `--it` | WP4 |
| `detail/tree/tree.hpp:284` | `find` returns a 24-byte optional through memory, outlined for every lookup | `get` 268.5 → 248.5 (1k), 417.6 → 397.6 (1M) | WP4 |
| `unsync/map.hpp:126` | `lower_bound`, `upper_bound`, `begin` copy the cursor and its node stack | 373.5 → 335.7 (1k) | WP4 |
| transparent lookups | a string literal (`char[N]`) or an `int` for a `u64` map takes the path without inline keys | 1,112 against 533 as `string_view` | WP4 |
| `detail/node/child.hpp:71-77` | the "branchless" binary search branches under Clang 15 (the Rust crate uses `select_unpredictable`): every map without inline keys (`int`, `double`, `std::greater`, `plain_keys`) | branch misses a lookup 5.7 (1k), 12.8 (1M) against 1.15 with GCC | WP1 |
| `unsync/map.hpp:353-366` | `insert_or_assign_many` looks every key up first, which the Rust crate skips below its batch cutoff and for appends | +63% an insert at 1k-28k, 4.4× for ascending keys at 1M | WP5 |
| `detail/tree/insert.hpp:26,57` | any insert that isn't an append forgets the remembered last leaf (the Rust crate too) | append and upsert pairs 1,154 → 725 | WP5 |
| `erase(find(k))` | descends twice (the node stack attached lazily) | 1,276 against 615 for `erase(k)` | WP5 |
| copy construction | entry by entry; a 4-entry map builds its root twice | 43 an entry at 1M, 968 a 4-entry map | WP5 |
| `erase(first, last)` | one erase an entry; `erase(begin(), end())` isn't `clear()` | 550 an entry | WP5 |
| `detail/node/leaf.hpp:277-292` | the bulk build's append rereads the leaf's length and keeps the shift's paths | 108 → 96.5 an entry | WP5 |
| leaf inserts | the inline keys' and entries' shifts are two `memmove` calls into libc, 24% of an insert's instructions | idea, not prototyped | WP5 |
| `detail/latched/latched_tree.hpp`, `optimistic.hpp` | shared lookups: `descend` and `find_unlatched` out of line, the routing's result through the stack (the edge index stored and reloaded on the chain to the child pointer), 40 bytes of the shared prefix copied where the Rust crate reads one | 1M `u64` hits: `read` 549 → 486, `get_copied` 591 → 537 (the Rust crate's 537), `contains` 587 → 536 | WP7b |
| `detail/latched/changes.hpp` | shared inserts and removals cost 230-270 more than the unsync map's, the Rust crate's 90-100 more | insert 867 against 595 (Rust 724 against 631) | WP7b |
| `detail/node/layout.hpp:115-133` | `compact_string` leaves have no hash tags (PORTING.md says they keep the length); one entry less a leaf would fit them | 13% more L1d misses a miss on path-like keys, confounded | not planned: at most 0.05% of the bench keys' leaf lookups have a second candidate |

## Work packages

| package | branch | covers | status |
|---|---|---|---|
| WP1 | `exp/explicit-count` | R1; the binary search that branches under Clang | [x] |
| WP2 | `exp/glibc-default` | R2 | [x] |
| WP3 | `exp/fair-comparison` | R3 | [x] |
| WP4 | `exp/iterators-lookups` | iterator steps, reverse iteration, `find`'s result, bounds, transparent lookups | [x] |
| WP5 | `exp/write-paths` | batched inserts, the remembered last leaf, `erase(find(k))`, copies, range erase, the bulk build's append, the shifts | [x] |
| WP6a | `exp/tests-unsync` | R4: the unsync map's and set's tests, with their fixes | [x] |
| WP6b | `exp/tests-file-maps` | R4, R8: file maps and bbtree-mmap, tests, fixes, a fuzz target | [x] |
| WP7a | `exp/shared-map-tests` | R4, R5: the scan's defect, switch points and the missing interleaving tests, the racing reads kept clean | [x] |
| WP7b | `exp/shared-map-perf` | shared lookups, inserts and removals against the Rust crate's instructions; PORTING.md's open point on shared hits, which compares `read()` here with an unlatched lookup on another CPU there | [x] |
| WP8 | `exp/store` | R6, R7, the store's leak, CRC32C | [x] |
| WP9 | | R2: a pooled default: not built, WP2 leaves no case for it | [x] |

## Results

Each package was cut short when the review's budget ran out: it finished what
it had in hand and says what it left. The packages' reports are with the
reviews'. Times are from quiet windows (1-minute load under 0.3 but
for WP3's, at 0.35), base and variant interleaved, two rounds; "i" is
instructions. Each branch stood on `main` at 8f53193 alone; "The merge"
below is what they are together.

| package | branch, commits | kept | not done |
|---|---|---|---|
| WP1 | `exp/explicit-count`, 1 | counts in explicit AVX2 or SSE2, no vectorizer and no POPCNT needed. Published build: `u64` get hit 1k 15.4 → 10.2 ns, miss 14.5 → 8.4, insert 29.7 → 23.8; 1M hit 85 → 69, miss 60 → 48; 16-byte strings' lookups 10-15% faster; 1M JSON keys inconclusive. g++ `-O2 -march=native`: `u64` get hit 1k 59 → 11.5 ns, insert 84 → 30, 1M hit 181 → 73: every cell 2-5.9× faster | the binary search that branches under Clang; g++ baseline timing (its 8-byte slots are branch-free now but run more instructions, 179 → 241 a count); the published tables want a rerun |
| WP2 | `exp/glibc-default`, 2 | nodes aligned to 64 bytes: on glibc 1M `u64` bulk build 24.9 → 15.3 ns, iterate 2.24 → 1.48, range64 465 → 332, insert 138 → 116, peak RSS 85.5 → 61.8 MB; 1M maps of 4: 2,285 → 1,370 MB; mimalloc unchanged. The tunable alone: insert 138 → 102, bulk build 24.9 → 10.8, walks unmoved; with both, the default allocator equals the pool. A moved-from `pooled_allocator` keeps its pool | the pool's mutex and chunk mapping. A pooled default: no case left |
| WP3 | `exp/fair-comparison`, 2 | the store against Rust built alike: insert 240-246 against 226-230 ns, get 219-221 against 204-212, scan 20.9-21.1 against 17.1, 8 threads even: COMPARISON.md's "inserts and gets are even or faster; scans are 5% slower" does not stand; "even with 8 threads" does. The bench no longer times the input's destruction (`build_many` 1.6-5.8% less); `--perf` counts on this CPU (the parser had mismatched perf's event name) | pinning and address-space randomization against the run-to-run range; the docs' corrections are drafted, not applied |
| WP4 | `exp/iterators-lookups`, 5 | reverse iterators of the maps' own (45 → 0.73 ns an entry at 1k); steps forced inline (two loops a map type: 1.9 → 0.7 ns an entry); `find` in two registers (16 i a hit less); bounds' cursors in place; literals and matching integers converted before the search (`contains("literal")` 65 → 40 ns) | one probe (a bound and 64 steps) got slower with the forced inlining, 79 → 85-88 ns, cause unknown; sanitizers not run |
| WP5 | `exp/write-paths`, 6 | the remembered leaf kept across upserts (append and upsert pairs at 1M 123-139 → 86-88 ns); bulk build (1k `u64` 2.78 → 1.96 ns an entry); batched inserts (1k map 75 → 50 ns a key, ascending at 1M 50 → 13); `erase(find(k))` (1k 71.7 → 36.6 ns, timed before its last fix); `erase` of a whole map or of a range at either end; copies of trivially copyable maps (1k 1.75 → 0.67 ns an entry) | the shifts' `memmove` calls; `erase(find(k))`'s final state untimed |
| WP6a | `exp/tests-unsync`, 12 | all ten unsync defects of Part 2 fixed, each with its test, and one more found: under keys that lie, `erase(find(k))` could remove another entry. Distinct test cases 146 → 274. Lookups' and steps' instructions unchanged; inserts +0.65%, unattributed | `BBTREE_CHECKED`, assertions in the sanitizer presets, six test groups, the README's deviations |
| WP6b | `exp/tests-file-maps`, 8 | a checked open's three holes closed, the fingerprint with the storage format, the writer's stale slots zeroed, `plain_bytes` opt-in. Files written before get another fingerprint | the mutated-image probe not run against the branch; the fuzz target unlanded; `uint32_t` → `std::string` files; R8's list |
| WP7a | `exp/shared-map-tests`, 4 | the scan's defect fixed, with the ported Shuttle test failing without the fix; switch points and a seeded scheduler (production code unchanged, byte for byte under Clang); `view()`'s read annotated; the memory clobber in `is_unchanged` (+1 to 2 i a shared insert) | six interleavings, the mutant table, the checker in CI and on the new code; the scheduler holds one `thread_local` pointer, against AGENTS.md's letter |
| WP7b | `exp/shared-map-perf`, 5 | shared lookups 11-19% fewer i (1M `get_copied` 554 → 492); the shared map's cost over the unsync map's at 1M: insert 236 → 138 i (Rust 93), removal 213 → 130 (Rust 100). 1k cells: random insert 35.4 → 30.6 ns, get miss 17.4 → 16.3; 8 threads inside noise | the racing-reads checker on the inlined code; Rust timings for PORTING.md's open point |
| WP8 | `exp/store`, 3 | a refused insert returned as a value: 20,724 → 1,842 i; evict and retry at the budget, 32-byte values 2,434 → 1,017 ns on one thread, 455 → 267 on eight; ordinary inserts unchanged under Clang. The overflow pages' leak fixed. The Rust crate's head race reproduced and written up for upstream | a head's record under its separator's try-latch (reads correct, unbuilt); CRC32C |

What the measurements say of the decisions:

- **R1** was the costliest: 2-5.9× under g++ `-O2` with a native or v2/v3
  target, and 15-45% in the published build.
- **R2:** the 1 KiB alignment costs glibc users a third of their memory and
  up to 1.6× in builds and walks, and mimalloc users nothing.
- **R3:** one of COMPARISON.md's four summary lines was made by the flags.
- **R4:** eleven defects by the ported tests alone.
- **R5:** clean today, and cheap to keep clean.
- **R6:** the decision was right; its cost is in pinned pages.
- **R7:** an exception a refusal more than doubles what a store at its
  budget takes an insert.

## The merge

The ten branches are one line of commits on `integration` (2026-11-10), the
measuring fixes first, then the fixes, then the performance changes, each
commit compiling; where two met (the cursor that keeps its root against the
cursor made in place; range erase in two branches) the fix's behavior and
the faster path are both kept. On top of them:

- **The defects the packages left:** `bbtree_map_file<uint32_t,
  std::string>` (a head's value placed also without inline keys), the
  shared map's `get_or_insert` with a closure that returns nothing and
  `extend` with entries that only move (with tests of `update_or_insert`,
  `remove_if` and `get_owned`, which had none), the root page a scan kept
  latched across its retry, `BBTREE_CHECKED` gating the checks, assertions
  on in the `asan`, `tsan` and `fuzz` presets (nothing failed with them
  on), `node_config.hpp`'s example.
- **Gates at the tip:** clang-format; 619 tests under Clang, GCC and
  ASan with UBSan, 616 under TSan, all with assertions; the eight fuzz
  targets two minutes each; the mutated file maps (8,800 images, no
  report); memory unchanged byte for byte over 48 cells; AArch64
  type-checked, its counts still NEON.
- **Instructions,** the merged tree against `main` with the bench's fixes,
  46 cells of `u64` and 16-byte string keys, the unsync and the shared map:
  none runs more, lookups 35-40% fewer, inserts and removals 21-31%, walks
  and `pop_first` the same. One probe outside the bench, a bound and 64
  steps in one loop, runs 4.4% more instructions and 1.4% more cycles than
  on `main` (the steps' forced inlining); the bench's `range64` runs 7-10%
  fewer.
- **Time:** COMPARISON.md and CPP_MAPS.md, remeasured in one evening's
  quiet windows with the Rust side built as its bench crate builds.

Open after the merge, each in FUTURE_WORK.md or PORTING.md:

- The racing-reads checker passes Clang's builds and can't follow GCC's,
  where `main` passed at `-O2`: GCC's builds rest on the version check's
  memory clobber alone (PORTING.md, [no checker](PORTING.md#p15-no-racing-reads-checker-on-the-compiled-code)).
- Not built: the binary search that branches under Clang, the pool's lock,
  CRC32C, a head's record under its separator's node, the leaf shifts.
- Not ported: six interleavings, node-level units and four more test
  groups, a fuzz target for file maps; bbtree-mmap's edge cases.
- g++ at `-O2` for baseline x86-64 was not timed; its string leaves' counts
  run more instructions than before, without branches.
- stx_btree was not measured against the merged tree.
