// The shared map against std::map on one thread, integer and string keys.
#include "../sync_map_differential.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 0 && data[0] % 2 == 0) {
    bbtree_fuzz::sync_map_differential<std::uint64_t>::run({data + 1, size - 1});
  } else if (size > 0) {
    bbtree_fuzz::sync_map_differential<std::string>::run({data + 1, size - 1});
  }
  return 0;
}
