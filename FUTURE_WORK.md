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
`node_inline_keys::equal_range`.

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

## The shared map's batches, reverse scans and copied ranges

The Rust crate's shared map has batched `read_many`, `get_many_copied`,
`get_many_cloned`, `insert_many` and `update_or_insert_many`,
`scan_rev` and `range_cloned`. Not ported yet: the bench drives the shared
map with single-key operations and forward scans only. The unsync map's
level-by-level lanes ([batches](PORTING.md#p10-batches-descend-level-by-level-not-in-async-lanes)) would carry over, each lane ending in
an optimistic leaf read.

## Unlatched copies of byte-string values

`get_copied` copies a value without latching its leaf only for keys
ordered by their own bytes (integers, byte arrays). The Rust crate also
settles a byte-string key that ends within its leaf's 8-byte inline key by
its hash tag, an exact length below 255, reading no heap byte. String maps
take the shared latch instead.

## Iterators whose steps stay in registers

Range-for steps keep the iterator's position in memory, because the leaf
crossings take the node stack inside the iterator by address: 1.6 ns an
entry with Clang against 0.9 for `for_each`. An iterator could keep its node
stack outside itself (a cursor handle into a stack the range owns), or keep
none and find the next leaf by a search from the root at each crossing,
using the internal nodes' separators. Either way, more iterator states
(attached, detached, at a separator) have to stay in sync with the cursor's
removal paths.
