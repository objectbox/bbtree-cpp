// The shared map's lookups and in-place updates from many threads: readers
// must only ever see values their keys could hold (the Rust crate's threads
// tests; Shuttle's place is taken by these stress runs under TSan, P1).
#include <bbtree/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

// A value encodes its key in its low 32 bits and a version above them.
constexpr std::uint64_t value_of(std::uint64_t key, std::uint64_t version) { return key | (version << 32); }

}  // namespace

TEST_CASE("readers see only values their keys held, while updates run", "[shared]") {
  constexpr std::uint64_t key_count = 50'000;
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
  for (std::uint64_t key = 0; key < key_count; ++key) {
    entries.emplace_back(key * 2, value_of(key * 2, 0));
  }
  map.extend(entries.begin(), entries.end());
  map.check_invariants(key_count);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> mismatches{0};
  std::vector<std::thread> threads;
  for (int reader = 0; reader < 4; ++reader) {
    threads.emplace_back([&, reader] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(reader));
      while (!stop.load(std::memory_order_relaxed)) {
        const std::uint64_t key = rng() % (key_count * 2);
        const auto copied = map.get_copied(key);
        const auto read = map.read(
            key, [](const std::uint64_t& k, const std::uint64_t& v) { return (v & 0xffffffff) == k; });
        const bool present = key % 2 == 0;
        if (copied.has_value() != present || read.has_value() != present || map.contains(key) != present) {
          mismatches.fetch_add(1);
        }
        if (copied && (*copied & 0xffffffff) != key) {
          mismatches.fetch_add(1);
        }
        if (read && !*read) {
          mismatches.fetch_add(1);
        }
      }
    });
  }
  for (int writer = 0; writer < 2; ++writer) {
    threads.emplace_back([&, writer] {
      std::mt19937_64 rng(100 + static_cast<std::uint64_t>(writer));
      for (int round = 0; round < 200'000; ++round) {
        const std::uint64_t key = rng() % key_count * 2;
        map.update(key, [](const std::uint64_t& k, std::uint64_t& v) { v = value_of(k, (v >> 32) + 1); });
      }
    });
  }
  threads[4].join();
  threads[5].join();
  stop.store(true);
  for (int reader = 0; reader < 4; ++reader) {
    threads[static_cast<std::size_t>(reader)].join();
  }
  CHECK(mismatches.load() == 0);
  map.check_invariants(key_count);
}

TEST_CASE("string keys read latched where the leaf can't settle them", "[shared]") {
  bbtree::bbtree_map<std::string, std::uint64_t, std::less<>> map;
  std::vector<std::pair<std::string, std::uint64_t>> entries;
  for (std::uint64_t i = 0; i < 20'000; ++i) {
    entries.emplace_back("/data/" + std::to_string(i % 13) + "/" + std::to_string(i), i);
  }
  map.extend(entries.begin(), entries.end());
  std::vector<std::thread> threads;
  std::atomic<int> mismatches{0};
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(t));
      for (int round = 0; round < 50'000; ++round) {
        const std::uint64_t i = rng() % 25'000;
        const std::string key = "/data/" + std::to_string(i % 13) + "/" + std::to_string(i);
        const auto value = map.get_copied(std::string_view(key));
        if (value.has_value() != (i < 20'000) || (value && *value != i)) {
          mismatches.fetch_add(1);
        }
        if (t % 2 == 0) {
          map.update(key, [](const std::string&, std::uint64_t&) {});
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(mismatches.load() == 0);
  map.check_invariants(20'000);
}

namespace {

// Distinct short keys whose 8-byte windows tie: a number and 0 to 3 zero
// bytes, so "12" and "1\0" share a window and differ in length; under a
// shared prefix, most end within a leaf's window and some don't.
std::string short_key(std::uint64_t i) { return "p" + std::to_string(i) + std::string(i % 4, '\0'); }

}  // namespace

TEST_CASE("short string keys copied unlatched match std::map, misses included", "[shared]") {
  bbtree::bbtree_map<std::string, std::uint64_t, std::less<>> map;
  std::map<std::string, std::uint64_t> expected;
  for (std::uint64_t i = 0; i < 6'000; i += 2) {
    map.insert(short_key(i), i);
    expected.insert_or_assign(short_key(i), i);
  }
  for (std::uint64_t i = 0; i < 6'000; ++i) {
    const std::string key = short_key(i);
    const auto found = expected.find(key);
    const auto copied = map.get_copied(std::string_view(key));
    REQUIRE(copied.has_value() == (found != expected.end()));
    if (copied) {
      REQUIRE(*copied == found->second);
    }
    // A key one zero byte longer ties in its window and differs in length.
    const std::string longer = key + std::string(1, '\0');
    REQUIRE(map.get_copied(std::string_view(longer)).has_value() == expected.contains(longer));
  }
}

TEST_CASE("short string keys copied unlatched while writers change their leaves", "[shared]") {
  bbtree::bbtree_map<std::string, std::uint64_t, std::less<>> map;
  constexpr std::uint64_t keys = 4'000;
  for (std::uint64_t i = 0; i < keys; ++i) {
    map.insert(short_key(i), value_of(i, 0));
  }
  std::atomic<bool> done{false};
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  // Writers: odd keys come and go, which splits and merges leaves; even keys
  // get new versions in place.
  for (int t = 0; t < 2; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(t) + 100);
      for (std::uint64_t round = 1; round < 40'000; ++round) {
        const std::uint64_t i = rng() % keys;
        if (i % 2 == 1) {
          if (rng() % 2 == 0) {
            map.remove(short_key(i));
          } else {
            map.insert(short_key(i), value_of(i, round));
          }
        } else {
          map.update(short_key(i),
                     [&](const std::string&, std::uint64_t& value) { value = value_of(i, round); });
        }
      }
      done.store(true);
    });
  }
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(t));
      while (!done.load()) {
        const std::uint64_t i = rng() % keys;
        const auto value = map.get_copied(std::string_view(short_key(i)));
        // Even keys are always there; any value read is its own key's.
        if ((i % 2 == 0 && !value) || (value && (*value & 0xffffffffu) != i)) {
          wrong.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(wrong.load() == 0);
}
