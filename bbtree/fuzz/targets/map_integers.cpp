// Map operations against std::map on integer keys (u64): 2-byte inline keys.
#include "../map_differential.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bbtree_fuzz::map_differential<std::uint64_t>::run({data, size});
  return 0;
}
