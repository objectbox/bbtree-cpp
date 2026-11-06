// Map operations against std::map on std::string keys: 8-byte leaf and 4-byte
// internal inline keys, hash tags, prefix stripping.
#include "../map_differential.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bbtree_fuzz::map_differential<std::string>::run({data, size});
  return 0;
}
