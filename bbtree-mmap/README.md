# bbtree-mmap

bbtree++ on disk: compact a bbtree++ map to a file, map it back into memory and
query it at once, with the map's own lookups
([15-index-file.md](../design/15-index-file.md)).

```cpp
#include <bbtree_mmap/bbtree_map_file.hpp>

bbtree::unsync::bbtree_map<std::uint64_t, std::string> names{{7, "ferris"}, {42, "corro"}};
bbtree_mmap::bbtree_map_file<std::uint64_t, std::string>::create("names.bbtree", names);

// Nothing may change the file while it is mapped.
auto file = bbtree_mmap::bbtree_map_file<std::uint64_t, std::string>::mmap("names.bbtree");
file.get(42);  // std::optional<std::string_view>{"corro"}
```

- Plain keys and values (integers, floats, byte arrays) stay in their slots;
  `std::string` and `std::vector<std::uint8_t>` become spans to bytes in heap
  pages right after their leaf, so a lookup reads one place of the file.
- `mmap` checks every page once, so that any bytes give answers or a
  `bbtree::file_error`, never undefined behavior; `mmap_unchecked` trusts
  the file. `read` reads it into memory instead. `os_pages::huge` asks for
  transparent huge pages.
- A file map is for one layout: the same key and value types, pointer width
  and byte order. C++ and Rust file maps are not interchangeable: the C++
  port's layouts differ where its types do (PORTING.md, [moves](../PORTING.md#p6-moves-relocate-only-trivially-relocatable-types-as-bytes)).

Examples, ports of the Rust crate's (the last three on mimalloc, as theirs
are, so built with the bench: `cmake --preset bench && cmake --build --preset
bench`, in `build/bench/bbtree-mmap/`):

- `bbtree_mmap_lookups [entries]` times lookups in a mapped file against the
  map it was written from.
- `bbtree_mmap_first_query [entries] [directory]`: the time from a restart to
  the first answer, out of the page cache and in it: `std::map` reads its
  entries back and builds itself, a `bbtree_map_file` maps its file and reads
  only the pages the lookup reaches. Prints CSV.
- `bbtree_mmap_huge_pages [entries] [directory]`: lookups in a file mapped
  from disk in base and in huge OS pages, against the map.
- `bbtree_mmap_var_bytes_lookups [entries]`: lookups of var-length keys and
  values, in heap pages after the leaves, against plain ones in the slots, and
  of text keys, each against a map.

`directory` must be on a disk, not tmpfs; it is the system's temporary one by
default.
