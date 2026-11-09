// Batches (D53): results match individual calls in input order, repeated keys
// included, on trees large enough for lanes and on small ones.
#include <bbtree/unsync/map.hpp>
#include <bbtree/unsync/set.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <random>
#include <string>

TEST_CASE("get_many matches get, on trees past the batch cutoff", "[batch]") {
  for (const std::size_t count : {10u, 1000u, 200'000u}) {
    bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map;
    std::mt19937_64 rng(count);
    for (std::size_t i = 0; i < count; ++i) {
      map.insert({rng() % (count * 2), i});
    }
    for (int round = 0; round < 200; ++round) {
      std::array<std::uint64_t, 64> keys;
      for (auto& key : keys) {
        key = rng() % (count * 2);
      }
      keys[7] = keys[3];
      const auto values = map.get_many(keys);
      for (std::size_t index = 0; index < keys.size(); ++index) {
        REQUIRE(values[index] == map.get(keys[index]));
      }
    }
  }
}

TEST_CASE("get_many on string keys, and contains_many on a set", "[batch]") {
  bbtree::unsync::bbtree_map<std::string, std::uint64_t, std::less<>> map;
  bbtree::unsync::bbtree_set<std::string> set;
  for (std::uint64_t i = 0; i < 100'000; ++i) {
    const std::string key = "/var/lib/data/" + std::to_string(i % 101) + "/" + std::to_string(i * 7);
    map.insert({key, i});
    set.insert(key);
  }
  std::mt19937_64 rng(1);
  for (int round = 0; round < 100; ++round) {
    std::array<std::string, 40> keys;
    for (auto& key : keys) {
      const std::uint64_t i = rng() % 140'000;
      key = "/var/lib/data/" + std::to_string(i % 101) + "/" + std::to_string(i * 7);
    }
    const auto values = map.get_many(keys);
    const auto held = set.contains_many(keys);
    for (std::size_t index = 0; index < keys.size(); ++index) {
      REQUIRE(values[index] == map.get(keys[index]));
      REQUIRE(held[index] == set.contains(keys[index]));
    }
  }
}

TEST_CASE("insert_or_assign_many matches inserts one by one", "[batch]") {
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> batched;
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> single;
  std::mt19937_64 rng(9);
  for (int round = 0; round < 5000; ++round) {
    std::array<std::pair<std::uint64_t, std::uint64_t>, 16> entries;
    for (auto& entry : entries) {
      entry = {rng() % 200'000, rng()};
    }
    entries[5].first = entries[2].first;
    std::array<bool, 16> expected;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      expected[index] = single.insert_or_assign(entries[index].first, entries[index].second).second;
    }
    REQUIRE(batched.insert_or_assign_many(std::move(entries)) == expected);
  }
  batched.check_invariants();
  REQUIRE(batched == single);
}

TEST_CASE("batched inserts of ascending keys, and of keys inside, past the batch cutoff", "[batch]") {
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> batched;
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> single;
  bbtree::unsync::bbtree_set<std::uint64_t> batched_set;
  bbtree::unsync::bbtree_set<std::uint64_t> single_set;
  std::mt19937_64 rng(10);
  std::uint64_t next = 0;
  for (int round = 0; round < 8000; ++round) {
    // Ascending past the last key: appends. Every eighth batch goes inside,
    // and every eighth of the others ends inside.
    std::array<std::pair<std::uint64_t, std::uint64_t>, 16> entries;
    std::array<std::uint64_t, 16> keys;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const bool inside = round % 8 == 7 || (round % 8 == 3 && index >= 12);
      keys[index] = inside ? rng() % (next * 4 + 1) : ++next * 4;
      entries[index] = {keys[index], rng()};
    }
    std::array<bool, 16> expected;
    std::array<bool, 16> expected_in_set;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      expected[index] = single.insert_or_assign(entries[index].first, entries[index].second).second;
      expected_in_set[index] = single_set.insert(keys[index]).second;
    }
    REQUIRE(batched.insert_or_assign_many(std::move(entries)) == expected);
    REQUIRE(batched_set.insert_many(std::move(keys)) == expected_in_set);
  }
  batched.check_invariants();
  batched_set.check_invariants();
  REQUIRE(batched.size() > 100'000);
  REQUIRE(batched == single);
  REQUIRE(batched_set == single_set);
}
