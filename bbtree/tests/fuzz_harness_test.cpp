// The fuzz harnesses on pseudo-random inputs of every length up to 16 KiB, the
// same each run (the Rust fuzz crate's `RandomInputs` test): every harness
// agrees with std on them, so that what the fuzzer reports is a bug to
// explain.
#include "../fuzz/hostile_keys.hpp"
#include "../fuzz/map_differential.hpp"
#include "../fuzz/set_algebra.hpp"
#include "../fuzz/sync_map_differential.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace {

template <class Harness>
void run_random_inputs(int count) {
  std::mt19937_64 rng(0x2545F4914F6CDD1D);
  for (int round = 0; round < count; ++round) {
    std::vector<std::uint8_t> bytes(rng() % 16'384);
    for (auto& byte : bytes) {
      byte = static_cast<std::uint8_t>(rng());
    }
    Harness::run(bytes);
  }
}

}  // namespace

TEST_CASE("the map harnesses agree with std::map on random inputs", "[fuzz]") {
  run_random_inputs<bbtree_fuzz::map_differential<std::uint64_t>>(20);
  run_random_inputs<bbtree_fuzz::map_differential<std::string>>(20);
  run_random_inputs<bbtree_fuzz::map_differential<std::array<std::uint8_t, 16>>>(20);
  run_random_inputs<bbtree_fuzz::map_differential<bbtree_fuzz::user_key>>(20);
  run_random_inputs<bbtree_fuzz::map_differential<std::string, bbtree::plain_keys>>(20);
}

TEST_CASE("the shared map, set algebra and hostile key harnesses on random inputs", "[fuzz]") {
  run_random_inputs<bbtree_fuzz::sync_map_differential<std::uint64_t>>(15);
  run_random_inputs<bbtree_fuzz::sync_map_differential<std::string>>(15);
  run_random_inputs<bbtree_fuzz::set_algebra>(20);
  run_random_inputs<bbtree_fuzz::hostile_keys>(20);
}
