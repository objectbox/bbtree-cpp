// Scans of the shared map: in key order from any start, alone and while
// writers change the tree around them.
#include <bbtree/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <random>
#include <thread>
#include <vector>

TEST_CASE("a scan visits the entries in order from any start", "[shared][scan]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  std::map<std::uint64_t, std::uint64_t> model;
  std::mt19937_64 rng(4);
  for (int i = 0; i < 30'000; ++i) {
    const std::uint64_t key = rng() % 100'000;
    map.insert_or_assign(key, key + 1);
    model.insert_or_assign(key, key + 1);
  }
  std::vector<std::uint64_t> all;
  map.scan([&](const std::uint64_t& key, const std::uint64_t& value) {
    REQUIRE(value == key + 1);
    all.push_back(key);
    return true;
  });
  REQUIRE(all.size() == model.size());
  REQUIRE(
      std::equal(all.begin(), all.end(), model.begin(), [](auto a, const auto& b) { return a == b.first; }));
  for (std::uint64_t start = 0; start < 100'500; start += 997) {
    std::vector<std::uint64_t> seen;
    map.scan_from(start, [&](const std::uint64_t& key, const std::uint64_t&) {
      seen.push_back(key);
      return seen.size() < 64;
    });
    auto expected = model.lower_bound(start);
    for (const std::uint64_t key : seen) {
      REQUIRE(expected != model.end());
      REQUIRE(key == expected->first);
      ++expected;
    }
    REQUIRE((seen.size() == 64 || expected == model.end()));
  }
}

TEST_CASE("scans stay in order and see stable keys once while writers run", "[shared][scan]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  // Even keys stay; odd keys come and go.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> stable;
  for (std::uint64_t key = 0; key < 40'000; key += 2) {
    stable.emplace_back(key, key);
  }
  map.extend(stable.begin(), stable.end());
  std::atomic<bool> stop{false};
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < 2; ++w) {
    threads.emplace_back([&, w] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(w));
      while (!stop.load(std::memory_order_relaxed)) {
        const std::uint64_t key = (rng() % 20'000) * 2 + 1;
        if (rng() % 2 == 0) {
          map.insert(key, key);
        } else {
          map.erase(key);
        }
      }
    });
  }
  for (int round = 0; round < 40; ++round) {
    std::uint64_t previous = 0;
    bool first = true;
    std::size_t stable_seen = 0;
    map.scan([&](const std::uint64_t& key, const std::uint64_t& value) {
      if ((!first && key <= previous) || value != key) {
        wrong.fetch_add(1);
      }
      first = false;
      previous = key;
      stable_seen += key % 2 == 0 ? 1 : 0;
      return true;
    });
    if (stable_seen != stable.size()) {
      wrong.fetch_add(1);
    }
  }
  stop.store(true);
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(wrong.load() == 0);
}
