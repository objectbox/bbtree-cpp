// Uses each installed library once: the unsync map on the pooled allocator,
// the shared map, a file map's header and the store's.
#include <bbtree/map.hpp>
#include <bbtree/pooled_allocator.hpp>
#include <bbtree/unsync/map.hpp>
#include <bbtree_mmap/bbtree_map_file.hpp>
#include <bbtree_store/store.hpp>

#include <cstdint>
#include <functional>
#include <string>

int main() {
  bbtree::unsync::bbtree_map<std::uint64_t, std::string, std::less<std::uint64_t>, bbtree::pooled_allocator>
      names;
  names[42] = "answer";
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> counts;
  counts.insert(1, 2);
#if defined(BBTREE_CHECKED)
  return 1;  // the checks must stay in the build tree
#else
  return names.at(42) == "answer" && counts.get_copied(1) == 2 ? 0 : 1;
#endif
}
