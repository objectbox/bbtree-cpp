// Map operations against std::map on keys with their own inline_key
// specialization: hash tags and comparisons from where keys differ (D43).
#include "../map_differential.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bbtree_fuzz::map_differential<bbtree_fuzz::user_key>::run({data, size});
  return 0;
}
