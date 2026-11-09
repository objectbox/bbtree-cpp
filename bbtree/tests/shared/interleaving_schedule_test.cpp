// The scheduler of the shared map's interleaving tests
// (support/interleaving_schedule.hpp), tested on its own: a schedule is a
// function of its seed, schedules differ, they reach an interleaving that
// loses an update and one that doesn't, and a thread that waits for a latch
// lets its holder run.
#include "../support/interleaving_schedule.hpp"

#include <bbtree/detail/latched/latch.hpp>
#include <bbtree/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace {

using bbtree_test::interleaving_exploration;
using bbtree_test::interleaving_schedule;

// What two threads that insert and remove the same keys leave of a schedule:
// its switch points, its trace, and the map's keys.
struct schedule_outcome {
  std::uint64_t steps;
  std::uint64_t trace;
  std::vector<std::uint64_t> keys;

  explicit schedule_outcome(std::uint64_t seed) {
    interleaving_schedule schedule(seed);
    bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
    const auto churn = [&map](std::uint64_t first) {
      return [&map, first] {
        for (std::uint64_t key = first; key < first + 80; ++key) {
          map.insert(key, first);
          map.erase(key ^ 1);
        }
      };
    };
    schedule.run({churn(0), churn(1), churn(40)});
    steps = schedule.steps();
    trace = schedule.trace();
    map.scan([&](const std::uint64_t& key, const std::uint64_t&) {
      keys.push_back(key);
      return true;
    });
  }

  friend bool operator==(const schedule_outcome&, const schedule_outcome&) = default;
};

}  // namespace

TEST_CASE("a schedule is a function of its seed", "[shared][interleaving]") {
  std::set<std::uint64_t> traces;
  std::set<std::vector<std::uint64_t>> outcomes;
  for (std::uint64_t seed = 0; seed < 24; ++seed) {
    const schedule_outcome first(seed);
    const schedule_outcome again(seed);
    REQUIRE(first == again);
    REQUIRE(first.steps > 0);
    traces.insert(first.trace);
    outcomes.insert(first.keys);
  }
  // Seeds differ in their schedules, and in what the threads' races leave.
  REQUIRE(traces.size() > 12);
  REQUIRE(outcomes.size() > 1);
}

TEST_CASE("schedules reach a lost update, and its absence", "[shared][interleaving]") {
  std::size_t lost = 0;
  std::size_t kept = 0;
  for (std::uint64_t seed = 0; seed < 200; ++seed) {
    interleaving_schedule schedule(seed);
    bbtree::detail::interleaved<std::atomic<int>> counter{0};
    const auto add_one = [&counter] {
      const int read = counter.load(std::memory_order_relaxed);
      counter.store(read + 1, std::memory_order_relaxed);
    };
    schedule.run({add_one, add_one});
    (counter.load(std::memory_order_relaxed) == 2 ? kept : lost) += 1;
  }
  REQUIRE(lost > 0);
  REQUIRE(kept > 0);
}

TEST_CASE("a thread that waits for a latch lets its holder run", "[shared][interleaving]") {
  interleaving_exploration{200}.run([](interleaving_schedule& schedule) {
    alignas(8) std::byte page[16] = {};
    bbtree::detail::node_latch latch(page);
    std::atomic<int> held{0};
    schedule.run({
        [&] {
          const bbtree::detail::latch_version version = latch.lock_exclusive();
          held.store(1, std::memory_order_relaxed);
          for (int step = 0; step < 50; ++step) {
            bbtree::detail::interleaving::switch_point();
          }
          held.store(0, std::memory_order_relaxed);
          latch.unlock_exclusive(version);
        },
        [&] {
          latch.lock_shared();
          schedule.expect(held.load(std::memory_order_relaxed) == 0, "a shared hold beside an exclusive one");
          latch.unlock_shared();
        },
    });
    // A wait is a switch to the holder, not a spin that the seed must end.
    REQUIRE(schedule.steps() < 2'000);
  });
}

TEST_CASE("a schedule keeps the first expectation that failed", "[shared][interleaving]") {
  interleaving_schedule schedule(7);
  schedule.run({
      [&] { schedule.expect(true, "holds"); },
      [&] { schedule.expect(true, "holds too"); },
  });
  REQUIRE(schedule.failure().empty());
  schedule.expect(false, "the first");
  schedule.expect(false, "the second");
  REQUIRE(schedule.failure() == "the first");
}
