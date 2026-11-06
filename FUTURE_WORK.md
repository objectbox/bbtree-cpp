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
