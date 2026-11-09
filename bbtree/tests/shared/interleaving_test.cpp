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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace {

using bbtree_test::interleaving_exploration;
using bbtree_test::interleaving_schedule;

using u64_map = bbtree::bbtree_map<std::uint64_t, std::uint64_t>;

// Internal nodes of 12 keys and leaves of 9 entries: four levels with a few
// thousand entries.
struct small_internal_nodes : bbtree::default_node_config {
  static constexpr std::size_t internal_page_size = 64;
};
struct wide_value {
  std::uint64_t words[12];
};
using tall_map =
    bbtree::bbtree_map<std::uint64_t, wide_value, std::less<std::uint64_t>, bbtree::global_allocator,
                       bbtree::configured<bbtree::auto_inline_keys, small_internal_nodes>>;

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

// Leaves split in the middle of a tree of four levels, and then another
// thread inserts a key that shrinks the shared prefix. Such a split holds
// neither the root nor a border node, so nothing orders its read of what
// every key shares with the shrink's write: the read is one of the racing
// reads, which ThreadSanitizer reports here if it is not marked as one. (In a
// tree of three levels every split holds the root.)
TEST_CASE("a shrink of the shared prefix follows splits that held no border node", "[shared][interleaving]") {
  interleaving_exploration{8}.run([](interleaving_schedule& schedule) {
    tall_map map;
    constexpr std::uint64_t steady = 12'000;
    constexpr std::uint64_t first_split = 5'000;
    constexpr std::uint64_t split = 40;
    for (std::uint64_t index = 0; index < steady; ++index) {
      REQUIRE(map.insert(index * 16, wide_value{}));
    }
    std::atomic<bool> leaves_split{false};
    schedule.run({
        [&] {
          for (std::uint64_t index = first_split; index < first_split + split; ++index) {
            schedule.expect(map.insert(index * 16 + 1, wide_value{}), "an insert of a new key found it held");
          }
          leaves_split.store(true, std::memory_order_relaxed);
        },
        [&] {
          // Waited for through the scheduler and a relaxed load: in no order
          // with the splits that ThreadSanitizer knows of.
          while (!leaves_split.load(std::memory_order_relaxed)) {
            bbtree::detail::interleaving::let_another_run();
          }
          schedule.expect(map.insert(std::uint64_t{1} << 50, wide_value{}),
                          "an insert of a new key found it held");
        },
    });
    map.check_invariants(steady + split + 1);
  });
}
