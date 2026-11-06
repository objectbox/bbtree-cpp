// The shared map's inserts and removals from many threads, splits, merges and
// root changes included: each thread works on keys of its own, so the end
// state is known exactly, and on keys all threads share, whose outcome is
// counted; then the tree must be well formed (the Rust crate's threads and
// Shuttle tests, P1).
#include <bbtree/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

template <class Map, class MakeKey>
void run_threads(Map& map, MakeKey make_key, int threads, int operations, std::uint64_t own_keys) {
  std::vector<std::map<std::uint64_t, std::uint64_t>> expected(static_cast<std::size_t>(threads));
  std::atomic<std::int64_t> shared_balance{0};
  // Catch2's assertions are not thread-safe: workers count failures.
  std::atomic<int> wrong{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(t) * 7 + 1);
      auto& mine = expected[static_cast<std::size_t>(t)];
      for (int op = 0; op < operations; ++op) {
        const std::uint64_t n = rng() % own_keys;
        // Thread t owns the numbers n * threads + t; shared numbers are odd
        // multiples past them all.
        const std::uint64_t own = n * static_cast<std::uint64_t>(threads) + static_cast<std::uint64_t>(t);
        switch (rng() % 6) {
          case 0:
          case 1:
          case 2:
            map.insert_or_assign(make_key(own), own);
            mine[own] = own;
            break;
          case 3:
          case 4: {
            const bool removed = map.erase(make_key(own));
            if (removed != (mine.erase(own) == 1)) {
              wrong.fetch_add(1);
            }
            break;
          }
          default: {
            const std::uint64_t shared = own_keys * static_cast<std::uint64_t>(threads) + rng() % 64;
            if (rng() % 2 == 0) {
              if (map.insert(make_key(shared), shared)) {
                shared_balance.fetch_add(1);
              }
            } else if (map.erase(make_key(shared))) {
              shared_balance.fetch_sub(1);
            }
          }
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  REQUIRE(wrong.load() == 0);
  std::size_t expected_length = static_cast<std::size_t>(shared_balance.load());
  for (const auto& mine : expected) {
    expected_length += mine.size();
    for (const auto& [key, value] : mine) {
      REQUIRE(map.get_cloned(make_key(key)) == value);
    }
  }
  map.check_invariants(expected_length);
}

}  // namespace

TEST_CASE("threads insert and remove u64 keys", "[shared]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  run_threads(
      map, [](std::uint64_t n) { return n * 2654435761u % 1'000'003; }, 4, 60'000, 8000);
}

TEST_CASE("threads insert and remove ascending keys, which append", "[shared]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  run_threads(
      map, [](std::uint64_t n) { return n; }, 4, 60'000, 8000);
}

TEST_CASE("threads insert and remove string keys that shrink the shared prefix", "[shared]") {
  bbtree::bbtree_map<std::string, std::uint64_t> map;
  run_threads(
      map,
      [](std::uint64_t n) {
        return (n % 50 == 0 ? std::string("/a/") : std::string("/srv/data/common/")) +
               std::to_string(n % 97) + "/" + std::to_string(n);
      },
      4, 30'000, 3000);
}

TEST_CASE("readers race writers that split and merge", "[shared]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  std::atomic<bool> stop{false};
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int r = 0; r < 3; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(r));
      while (!stop.load(std::memory_order_relaxed)) {
        const std::uint64_t key = rng() % 40'000;
        if (const auto value = map.get_copied(key); value && *value != key * 3) {
          wrong.fetch_add(1);
        }
        if (const auto value = map.read(key, [](const auto&, const auto& v) { return v; });
            value && *value != key * 3) {
          wrong.fetch_add(1);
        }
      }
    });
  }
  for (int w = 0; w < 2; ++w) {
    threads.emplace_back([&, w] {
      std::mt19937_64 rng(10 + static_cast<std::uint64_t>(w));
      for (int op = 0; op < 150'000; ++op) {
        const std::uint64_t key = rng() % 40'000;
        if (rng() % 2 == 0) {
          map.insert(key, key * 3);
        } else {
          map.erase(key);
        }
      }
    });
  }
  threads[3].join();
  threads[4].join();
  stop.store(true);
  for (int r = 0; r < 3; ++r) {
    threads[static_cast<std::size_t>(r)].join();
  }
  CHECK(wrong.load() == 0);
  std::size_t count = 0;
  map.for_each([&](const auto&, auto&) { ++count; });
  map.check_invariants(count);
}

TEST_CASE("the shared map moves and swaps, its free pages with it", "[shared]") {
  using map_type = bbtree::bbtree_map<std::uint64_t, std::uint64_t>;
  const auto count = [](map_type& map) {
    std::size_t entries = 0;
    map.for_each([&](const auto& key, auto& value) {
      CHECK(value == key + 1);
      ++entries;
    });
    map.check_invariants(entries);
    return entries;
  };
  map_type first;
  for (std::uint64_t key = 0; key < 20'000; ++key) {
    first.insert(key, key + 1);
  }
  for (std::uint64_t key = 0; key < 20'000; key += 2) {
    first.erase(key);  // frees nodes, which stay on the map's free lists
  }
  map_type second(std::move(first));
  CHECK(count(second) == 10'000);
  CHECK(count(first) == 0);
  first.insert(7, 8);
  swap(first, second);
  CHECK(count(first) == 10'000);
  CHECK(second.get_copied(7) == 8);
  second = std::move(first);
  CHECK(count(second) == 10'000);
  CHECK(count(first) == 0);
  for (std::uint64_t key = 0; key < 20'000; key += 2) {
    second.insert(key, key + 1);  // reuses the moved free pages
  }
  CHECK(count(second) == 20'000);
}
