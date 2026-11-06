// Set algebra against std's set algorithms.
#include "../set_algebra.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bbtree_fuzz::set_algebra::run({data, size});
  return 0;
}
