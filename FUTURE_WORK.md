# Future work

Ideas that would make bbtree++ faster or more complete at the cost of more
complex code, or that the port left for later. Each says what it would gain,
what it would cost, and what was measured, if anything.

## Explicit AVX-512 and NEON counts

The Rust crate counts 2- and 4-byte inline keys, and 28 to 31 eight-byte
ones, in explicit AVX-512 vectors, takes an 8-byte equality mask for exact
lookups, and counts every width in NEON on AArch64 (a 10M `u64` hit 389 →
173 ns on an M1 Pro). Not ported (PORTING.md, [in-node counts](PORTING.md#p9-in-node-counts-the-compilers-sse2-masks-and-no-avx-512-or-neon)): no hardware
here to test or measure them. Each is one more specialization of
`node_inline_keys::equal_range`. On AArch64 the hash tags take a scalar
loop too (PORTING.md, [AArch64 and macOS](PORTING.md#p33-aarch64-and-macos-built-and-checked-not-measured)): a NEON compare and a narrowing
shift give 4 bits a tag, 64 bits for 16 tags, in place of SSE2's movemask.

## Saved destinations for batched inserts

`insert_or_assign_many` descends once to warm every key's path, then
inserts one by one, searching again. The Rust crate saves each lane's
destination leaf and its length, re-searching only leaves an earlier insert
changed and dropping destinations a split invalidates (12-batch-api.md,
"Insertion and staleness"). It would save the second search's instructions,
at the cost of the staleness bookkeeping.

## Keep a batch lane's query between stages

A lane rebuilds its `search_key` at each stage, which rereads a string
query's length and pointer. The Rust crate keeps the query in the lane
(12-batch-api.md, rule 5). It matters for string keys only.

## The shared map's batches and copied ranges

The Rust crate's shared map has batched `read_many`, `get_many_copied`,
`get_many_cloned`, `insert_many` and `update_or_insert_many`, and
`range_cloned`. Not ported yet: the bench drives the shared map with
single-key operations and scans only. The unsync map's
level-by-level lanes ([batches](PORTING.md#p10-batches-descend-level-by-level-not-in-async-lanes)) would carry over, each lane ending in
an optimistic leaf read.

## Reads that latch the leaf they read

`read`, which runs a closure on the entry, latches its leaf on the way
down (`descend<leaf_mode::shared>`), so a miss latches too. The Rust crate
reads the leaf optimistically first, settles misses unlatched and latches
the leaf it read only for a hit (`find_optimistic`), as `get_copied` and
`contains` do here (PORTING.md, [latching the leaf read](PORTING.md#p42-a-lookup-the-leaf-cant-settle-latches-that-leaf-not-the-root-down)).
Tried: `read` descending optimistically, settling a miss its inline keys
rule out, and latching the leaf it read otherwise. On a probe of 1M `u64`
keys it was slower, hits 77-97 → 110-128 ns and misses 76-99 → 115-146
single-threaded: with 2-byte windows few misses are ruled out, and the
latched descent already latches the leaf once, at its end. Not kept.

## bbtree-store: a head's record moved under its separator's node

A leaf's head key's record stays where it is (PORTING.md, [heads' records](PORTING.md#p24-a-leafs-head-keys-record-stays-put)), which pins its
page: all of the store's 7 bytes an entry and 0.22 bytes of heap a live
byte over the Rust crate's are holes in such pages, and compactions run
45-59% more often. The map's rule is that a separator's key is held by
holding its internal node. A store that moved a head's record only while it
also held that node, the lowest of its descent with an edge left of the
path, taken without waiting by a compare-and-swap at the version the descent
read, would obey it: no deadlock, as the thread waits for nothing while it
holds its leaf, and a swap that fails leaves the record pinned that time.
The map would hand the store's closures that node and version. By reading
it is correct; not built, and what it costs eight threads is unmeasured.

## bbtree-store: heads' stubs go once their key is no longer a head

A head key whose value outgrew its record keeps a stub and a target
(PORTING.md, [heads' records](PORTING.md#p24-a-leafs-head-keys-record-stays-put)) until its key is written again or removed. A compaction
could point a non-head key's stub's key to its target and kill the stub,
saving the stub's bytes and the indirection on reads.

## bbtree-store: smaller records for tiny values

Every record keeps 8 bytes after its key so that a head's record can become
a stub in place ([heads' records](PORTING.md#p24-a-leafs-head-keys-record-stays-put)): up to 8 bytes more than the Rust crate's for values
under 8 bytes. A record could keep less, and a head with a tiny record grow
by another route (a stub in a record of its own whose old bytes die only
once no separator's reader can hold them), at the cost of a second path.

## CRC32C in hardware

The WAL's CRC32C is a byte-at-a-time table ([CRC32C](PORTING.md#p27-crc32c-a-table-a-byte-at-a-time)), about 5 to 6
cycles a byte through its dependent load. It is paid by every `read_value`
and by the whole WAL at open, not once a commit. SSE4.2's `crc32`
instruction (8 bytes a cycle), or slicing by 8, which needs no CPU check,
would cut both. Not measured.

## libstdc++ strings moved as bytes, their short-string pointer fixed

libstdc++'s `std::string` points into itself while short, so `std::string`
keys shift one object at a time. Moving a run of them as bytes, then
pointing each short string's data pointer back at its own buffer, made
building 100k maps of 32 `std::string` → 32-byte entries 26% faster (79 ns
an insert to 58) in a prototype that fixed up `slots`' shifts and the paired
entries' shift; 1M random inserts into one map gained nothing, their shifts
overlapping the leaf's cache misses. The bench's `build_many_100kx32` cell
is where bbtree++ with `std::string` keys trails the Rust crate most (1.5×). Doing it means: a
relocation trait with a fix-up after byte moves (`std::string` under
libstdc++'s C++11 ABI, whose layout the ABI freezes: data pointer first,
buffer at 16), fix-ups in every byte-moving path (`slots`' shifts and runs,
`entry_shift`, whole-leaf copies), a test that checks the layout, and
copying the bytes of an object that isn't trivially copyable, which the
standard leaves undefined.

## `std::string` keys stored as compact strings

`bbtree::compact_string` keys need the map's key type to say so. A
`bbtree_map<std::string, V>` that stored them anyway would make the switch
transparent: `entry_ref` would hand out a proxy key that converts to
`std::string_view` and `std::string` in place of `const std::string&`, and
the nodes' plan would be the compact type's. It changes what `*it` is for
every map, so code that binds `const std::string&` to a key, or passes
`it->first` where a `std::string&` is wanted, stops compiling: a deviation
from std::map that the opt-in key type avoids.

## `compact_string` with 14 bytes in place and no length cap

`basic_compact_string<std::uint16_t, 14>` holds 14 bytes in place (90.3% of
the bench's JSON keys against 83.9%) and was measured: misses in 1M maps
9% faster, the rest even, but strings capped at 65,535 bytes. Lifting the
cap would take 0xFFFF as "the size is on the heap before the bytes": a
branch in `size()` for long strings only, and a second layout to test.

## From abseil's B-tree

`absl::btree_map` beats bbtree++ on 1k random u64 inserts (17 ns against
23.5), `pop_first` (2×), and on building 4-entry maps (CPP_MAPS.md). The Rust crate
trails `BTreeMap` the same way, so these are design differences, not port
ones. Three of abseil's choices explain them:

- **Root leaves sized to their entries.** abseil allocates a root leaf for
  as many slots as the map holds, and reallocates it larger as it grows,
  up to a full node: a 4-entry `std::string` → 32-byte map takes 88 bytes
  an entry against bbtree++'s 275, a full 1 KiB page. Building 1M such maps
  takes abseil 48-52 ns an entry against 66 (compact keys) to 77.
  Iterating them no longer shows it once the root leaf's loads follow a
  stride the prefetcher takes ([iteration](PORTING.md#p16-for_each-beside-iterators-for-rusts-folds)): compact keys 3.2 ns an entry against
  abseil's 3.5, `std::string` keys 5.6. For bbtree++ it means a root page of
  several sizes, a layout per capacity (the inline keys' area scales with
  it), and a copy each time the root grows; and it breaks the design's rule
  that "the root page stays the root", which the shared map's racing reads rely on, so it
  would be the unsync map's and set's only.
- **Parent pointers.** abseil's iterator is a node and an index, stepping
  through parent pointers; bbtree++'s keeps a node stack, which its
  forward steps keep beside a position in registers ([forward steps](PORTING.md#p38-a-step-is-inlined-whole-so-the-position-stays-in-registers)),
  and which `pop_first`'s descent builds. Parent pointers cost 8 bytes a node and a
  write to every child an internal split or merge moves, and the shared
  map's nodes would need them kept under its latches.
- **256-byte nodes.** abseil sizes nodes to about 256 bytes, so a u64 →
  u64 leaf holds 15 entries and an insert shifts at most 240 bytes. A
  bbtree++ leaf is at least 1 KiB (the design's floor, at least 12 keys): 55 u64
  entries, and up to 880 bytes shifted, which a 1k map in cache pays on
  every random insert and removal. Smaller leaves for small entries would
  need the 1 KiB floor and the inline-key area's sizing revisited.

## Bulk appends from set algebra and file maps

`tree_builder::extend` keeps a leaf's run in locals, which took a 1M u64
bulk build from 24.5 to 16.4 cycles an entry. The set algebra
(`unsync/set.hpp`'s merged builds) and the file map's loading
(`file.hpp`) still call `push` per entry, which reads the builder's
members back after each store into the page. Feeding them through
`extend` needs an iterator, or a callback form of it, over their merged
or decoded entries.

Tried with a source form of `extend` (an object with `has_next()` and
`take()`, which the iterator form wraps; an `std::optional`-returning
callback cost 16-byte string bulk builds 8%, a move more an entry), fed by
the map's `append` merge and the set algebra's merged and filtered builds.
A probe of two 1M `u64` sets: union 11.1-11.6 → 9.8 ns an entry,
intersection 4.9-5.1 → 5.5 (mostly skipping, which the source's state
made dearer), symmetric difference 9.2 either way; `append` of two 1M
maps 29.7 → 29.4-30.4, its time in the cursors' steps, the moves and the
old trees' teardown, not in the build. Not kept: no gain where the entry
expected one.

## The shared prefix of lookups

A lookup checks its key against what every key of the tree shares
(`shared_prefix::place_outside`, `cmp_shared_prefix(bytes())`), from a
span. A prefix shorter than 8 bytes, as maps of dense ids share, took a
`memcpy` call into a buffer and a stalled load back; since short windows
come from two overlapping loads (PORTING.md, [short windows](PORTING.md#p35-a-short-byte-strings-window-overlapping-loads-not-a-copy)), it
takes no call. What is left is reading the prefix through a span where
`shared_prefix::word_at` has it as one load; the bench's random keys share
no prefix, so no cell shows it.

## A pooled allocator that gives back free chunks

`pooled_allocator` (PORTING.md, [pooled pages](PORTING.md#p32-pooled_allocator-pages-packed-into-chunks-opt-in)) keeps every chunk until its last copy goes, so a
map that grows to 10M entries and shrinks to 10 holds its peak's pages.
Giving back a chunk whose pages are all free takes a count per chunk and,
on every free, finding the page's chunk: a search over the chunks, or
chunks aligned to their size so that the address names it, which 2 MiB
chunks already are. The free list would then have to drop the chunk's
pages, which a list per chunk does at the cost of choosing a chunk on each
allocation. Worth it for long-lived maps whose size swings by orders of
magnitude; not measured.

## The pooled allocator against mimalloc's on the shared map

With 8 threads inserting into one shared map, the pool's inserts took
49-54 ns where mimalloc's took 40-42 (PORTING.md, [pooled pages](PORTING.md#p32-pooled_allocator-pages-packed-into-chunks-opt-in)), with
both on huge pages (mimalloc's arena backed whole, the pool's chunks since
they are written at mapping) and about as many dTLB misses and page
faults; on one thread they matched. The pool takes one mutex for every
page, which a profile put under 0.1%, so the gap is elsewhere, perhaps in
where the pages land: mimalloc gives each thread pages of its own, the pool
one sequence for all. Not looked into further; worth it only for shared
maps that insert from many threads on the pool.

## `pop_first`: the leaf's shift, not the steal

abseil's B-tree pops the first entry 2× faster (CPP_MAPS.md). A pop takes
out a leaf's slot 0, which shifts the rest of the leaf, about 700 bytes of a
`u64` → `u64` leaf, by `memmove` on every pop: most of the time a 1M pop
takes past its descent. A leaf at its minimum also takes one entry from its
right sibling each pop, but moving several at once (as abseil rebalances
by half the difference) would leave the shift. A leaf whose live slots
start at an offset, so that taking slot 0 moves the offset, would remove
it, at the cost of an offset in every slot access.

## The binary search that branches under Clang

Maps without inline keys (`int`, `double`, `std::greater`, `plain_keys`) and
the kernel target search a node's keys by a binary search written without
branches (`detail/node/child.hpp`), which Clang 15 compiles to branches:
5.7 branch misses a `u32` lookup at 1k and 12.8 at 1M, against 1.15 under
GCC and 1.17 in the Rust crate, which uses `select_unpredictable`.
`__builtin_unpredictable` and arithmetic selects don't change Clang's mind;
steps written out without a loop, each guarded, do (0.46 misses at 1k in a
prototype). No bench cell has such a map.

## A leaf insert's two shifts

An insert into a leaf shifts its inline keys (up to 110 bytes) and its
entries (up to 880) by two calls of libc's `memmove`: 24% of the
instructions and of the branch misses of a `u64` insert and erase. A
shift of the small column in fixed-size vector moves would save one call.
Idea only; this is where abseil's 256-byte nodes lead at 1k (CPP_MAPS.md).

## The racing-reads checker for GCC, and in CI

The Rust crate's checker, adapted, passes Clang's builds of the shared map
and can't follow GCC's since the version check clobbers memory and the
descent is inlined (PORTING.md, [no checker](PORTING.md#p15-no-racing-reads-checker-on-the-compiled-code)): GCC loads a node's version through
a register that holds the latch's address. Teaching the checker that
address (a `lea` of offset 8 from a node pointer, followed through moves)
would let it check GCC's builds; then it could run in CI with its planted
reloads, which it must flag. AArch64 was never audited.

## The pool's lock

`pooled_allocator` zeroes a page and first touches a 2 MiB chunk under its
mutex, and with 8 threads inserting, threads sleep on it: 15 thousand
voluntary context switches per 200 thousand page requests, which the CPU
profile that cleared the mutex could not show. It is the likely rest of the
pool's gap to mimalloc on the shared map (the entry above). It also maps
4 MiB and trims for each 2 MiB chunk, three system calls and a hole each.

## Tests still to port

Of the Rust crate's tests, still without a counterpart: node-level units
(`slots`, `leaf_node`, `internal_node`, siblings, teardown; about 2,300
lines), hash tags at saturation, shared-prefix shrinks, inline-key hooks,
node configs, over-aligned and empty types (a reviewer's ports of these
pass, outside the repository); six of the seven Shuttle interleavings the
stress tests can't reach (root lowering under scans and under lookups,
three levels lowering to two, emptying the tree, a prefix shrink racing a
root raise, the root's separator removed while lowering), and merges,
steals and page reuse under threads, on the scheduler of PORTING.md's
[switch points](PORTING.md#p45-switch-points-in-the-latched-tree-and-a-scheduler-of-threads-for-tests); the file maps' round trips by key type and exact refusals;
a libFuzzer target that mutates valid file maps (a probe of 8,800 mutated
images found nothing after the checks of PORTING.md, [file maps](PORTING.md#p46-file-maps-what-a-checked-open-refuses-and-what-the-bytes-are)). The fuzz
harnesses drive `u64` values only, no batches and no overlapping `append`.
bbtree-mmap's edge cases (a range past the file's end, a 0-byte file, a
moved-from `file_map`'s `size()`) have no test and no fix.

