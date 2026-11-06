// Keys whose order and inline keys lie: memory-safe answers, under ASan.
#include "../hostile_keys.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bbtree_fuzz::hostile_keys::run({data, size});
  return 0;
}
