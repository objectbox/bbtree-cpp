// Map operations against std::map on 16-byte array keys: 2-byte inline keys
// and whole-key equality (D32, D38).
#include "../map_differential.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bbtree_fuzz::map_differential<std::array<std::uint8_t, 16>>::run({data, size});
  return 0;
}
