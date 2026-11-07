# bbtree-store

An ordered store of byte strings that threads share, whose every byte lies
in pages from its allocator: bbtree++'s shared map, whose keys point to their
records, key and value, kept in chains of heap pages beside each leaf (a
leaf annex; [16-bbtree-store.md](../design/16-bbtree-store.md)). An
allocator that refuses pages past a budget bounds all the memory a store
takes; past it, the store refuses inserts and holds what it held.

```cpp
#include <bbtree_store/store.hpp>

using bbtree_store::bytes_of;

bbtree_store::bbtree_store<> store;                 // threads share it by reference
store.insert(bytes_of("apple"), bytes_of("red"));   // insert_result::inserted
store.get(bytes_of("apple"));                       // std::optional{the bytes of "red"}
store.scan(bbtree_store::key_range::all(), [](auto key, auto value) {
  return true;                                      // false stops the scan
});
store.remove(bytes_of("apple"));
```

- Keys up to a quarter of a heap page (256 bytes at the default 1 KiB),
  values up to 2^32 - 1 bytes; values too long for a record go into overflow
  pages of their own.
- `get` and `get_into` copy the value out; `scan` and `scan_rev` show each
  entry while its leaf is latched; `range` hands out copies from either end,
  a few at a time, holding no latch between calls.
- A budget is an allocator (`bbtree::page_allocator`) whose `allocate_heap`,
  `allocate_leaf` and `allocate_internal` refuse pages past it: `insert`
  returns `insert_result::refused`, and removals take their holes back
  without a page.

## WAL

`bbtree_store::wal` (`<bbtree_store/wal/writer.hpp>`,
[01-wal.md](../design/bbtree-store/01-wal.md)) is the store's write-ahead
log: records with an LSN and a CRC32C, appended to numbered WAL files under
S3 Express's rules (append at the end only, whole or nothing, 10,000 appends
a file). `wal_writer::open` replays the WAL and claims it, fencing the
writer before; `append` writes one append; `record_address::read_value`
reads a record back. Storage is anything that models `wal::wal_storage`;
calls are synchronous (PORTING.md, [the synchronous WAL](../PORTING.md#p25-the-wal-is-synchronous)). The files' bytes are the Rust
crate's format, byte for byte.

## Differences from the Rust crate

PORTING.md, from [what is ported](../PORTING.md#p23-bbtree-store-what-is-ported-and-its-api-in-c) on: spans for keys and values and an `insert_result`
for `Result<bool, InsertError>`; a leaf's head key's record stays put, a
fix for a race the Rust store shares ([heads' records](../PORTING.md#p24-a-leafs-head-keys-record-stays-put)); a synchronous WAL whose errors
are exceptions; the Verus proofs become tests.
