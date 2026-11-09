// Races of the shared map that real threads reach once in millions of runs,
// under a scheduler that runs the threads one at a time and switches between
// them at the latched tree's switch points (support/interleaving_schedule.hpp;
// the Rust crate's `map/shuttle_tests.rs`). Each test sets a tree up one
// operation away from the change it races, and runs two threads of a few
// operations over it, under many schedules.
#include "../support/interleaving_schedule.hpp"

#include <bbtree/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using bbtree_test::interleaving_exploration;
using bbtree_test::interleaving_schedule;

using u64_map = bbtree::bbtree_map<std::uint64_t, std::uint64_t>;

// The keys a scan bounded by `low` and `high` visits, in key order: forward
// from `low` until a key past `high`, or in reverse from `high` back until a
// key before `low`.
std::vector<std::uint64_t> scanned_between(const u64_map& map, std::uint64_t low, std::uint64_t high,
                                           bool reverse) {
  std::vector<std::uint64_t> visited;
  if (reverse) {
    map.scan_rev_from(high, [&](const std::uint64_t& key, const std::uint64_t&) {
      if (key < low) {
        return false;
      }
      visited.push_back(key);
      return true;
    });
    std::reverse(visited.begin(), visited.end());
  } else {
    map.scan_from(low, [&](const std::uint64_t& key, const std::uint64_t&) {
      if (key > high) {
        return false;
      }
      visited.push_back(key);
      return true;
    });
  }
  return visited;
}

}  // namespace

// Scans, both ways, between bounds that lack what every key shares, just
// outside the keys, while another thread inserts a key past each bound, in
// either order, the first shrinking the shared prefix; in a root leaf, then
// in a tree of two levels. A scan that read its starting bound as before
// every key must not start at the first entry once the shrink has put a key
// there (the Rust crate's `scans_from_outside_the_shared_prefix_race_a_shrink`).
TEST_CASE("scans from outside the shared prefix race a shrink", "[shared][interleaving]") {
  for (const std::uint64_t len : {std::uint64_t{4}, std::uint64_t{200}}) {
    interleaving_exploration{4'000}.run([len](interleaving_schedule& schedule) {
      u64_map map;
      constexpr std::uint64_t base = std::uint64_t{0xAB} << 40;
      std::vector<std::uint64_t> steady;
      for (std::uint64_t index = 0; index < len; ++index) {
        steady.push_back(base + 10 * (index + 1));
        REQUIRE(map.insert(steady.back(), index));
      }
      // Each differs from every key in the third byte, which the keys share.
      constexpr std::uint64_t below = std::uint64_t{0xAA} << 40;
      constexpr std::uint64_t above = std::uint64_t{0xAC} << 40;
      std::array<std::uint64_t, 2> outside = {std::uint64_t{0xA9} << 40, std::uint64_t{0xAD} << 40};
      if (schedule.draw(2) == 1) {
        std::swap(outside[0], outside[1]);
      }
      schedule.run({
          [&] {
            for (const std::uint64_t key : outside) {
              schedule.expect(map.insert(key, 0), "an insert of a new key found it held");
            }
          },
          [&] {
            for (const bool reverse : {false, true}) {
              schedule.expect(scanned_between(map, below, above, reverse) == steady,
                              "a scan between bounds outside what every key shares visited other than "
                              "the keys between them");
            }
          },
      });
      map.check_invariants(steady.size() + 2);
    });
  }
}
