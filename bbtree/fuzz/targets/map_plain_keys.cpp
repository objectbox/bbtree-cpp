// Map operations against std::map on string keys without inline keys:
// internal nodes that keep their keys (D17).
#include "../map_differential.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bbtree_fuzz::map_differential<std::string, bbtree::plain_keys>::run({data, size});
  return 0;
}
