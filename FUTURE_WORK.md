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

## The shared map's batches and copied ranges

The Rust crate's shared map has batched `read_many`, `get_many_copied`,
`get_many_cloned`, `insert_many` and `update_or_insert_many`, and
`range_cloned`. Not ported yet: the bench drives the shared map with
single-key operations and scans only. The unsync map's
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

## bbtree-store: a refused insert without an exception

A store whose budget is spent refuses inserts by an `alloc_error` thrown
from its closure inside the map and caught by `insert` (PORTING.md, [refused inserts](PORTING.md#p21-refusing-an-insert-by-throwing-not-by-a-result-type)),
microseconds a refusal. A cache that runs at its budget refuses often; a
refusal result threaded through `latched_changes`' insert paths, and a
non-throwing failure of the split's page reservation, would make it as
cheap as an insert. Not measured.

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

The WAL's CRC32C is a byte-at-a-time table ([CRC32C](PORTING.md#p27-crc32c-a-table-a-byte-at-a-time)). SSE4.2's `crc32`
instruction (8 bytes a cycle), or slicing by 8, would cut the CRC of a
commit of many records, if appends ever show it.

## libstdc++ strings moved as bytes, their short-string pointer fixed

libstdc++'s `std::string` points into itself while short, so `std::string`
keys shift one object at a time. Moving a run of them as bytes, then
pointing each short string's data pointer back at its own buffer, made
building 100k maps of 32 `std::string` → 32-byte entries 26% faster (79 ns
an insert to 58) in a prototype that fixed up `slots`' shifts and the paired
entries' shift; 1M random inserts into one map gained nothing, their shifts
overlapping the leaf's cache misses. The bench's `build_many_100kx32` cell
is where the port trails the Rust crate most (1.7x). Doing it means: a
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
