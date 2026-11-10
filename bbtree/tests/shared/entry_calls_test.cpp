// The shared map's calls that run a closure on one entry, or hand the owner
// its entries: `get_or_insert`, `update_or_insert`, `remove_if`, `get_owned`
// and `extend`, against std::map across splits and merges. (The Rust crate's
// `map/tests.rs`: `get_or_insert_matches_std`, `update_or_insert_matches_std`,
// `remove_if_removes_only_what_its_condition_holds_of`.)
#include <bbtree/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

TEST_CASE("get_or_insert runs its closure on the entry the map keeps", "[shared]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  std::map<std::uint64_t, std::uint64_t> model;
  std::mt19937_64 rng(11);
  for (int step = 0; step < 30'000; ++step) {
    const std::uint64_t key = rng() % 6'000;
    const std::uint64_t value = rng();
    const std::uint64_t kept = model.try_emplace(key, value).first->second;
    if (step % 2 == 0) {
      const std::uint64_t seen = map.get_or_insert(
          key, value, [](const std::uint64_t&, const std::uint64_t& stored) { return stored; });
      REQUIRE(seen == kept);
    } else {
      // A closure that returns nothing: the call returns nothing either.
      std::uint64_t seen = 0;
      map.get_or_insert(key, value,
                        [&](const std::uint64_t&, const std::uint64_t& stored) { seen = stored; });
      REQUIRE(seen == kept);
    }
  }
  for (const auto& [key, value] : model) {
    REQUIRE(map.get_copied(key) == std::optional<std::uint64_t>(value));
  }
}

TEST_CASE("update_or_insert merges into an entry or inserts one", "[shared]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  std::map<std::uint64_t, std::uint64_t> model;
  std::mt19937_64 rng(12);
  for (int step = 0; step < 30'000; ++step) {
    const std::uint64_t key = rng() % 6'000;
    const std::uint64_t value = rng() % 1'000;
    const auto [at, inserted] = model.try_emplace(key, value);
    if (!inserted) {
      at->second += value;
    }
    const std::optional<std::uint64_t> merged = map.update_or_insert(
        key, value,
        [](const std::uint64_t&, std::uint64_t& stored, std::uint64_t&& with) { return stored += with; });
    REQUIRE(merged.has_value() == !inserted);
    if (merged) {
      REQUIRE(*merged == at->second);
    }
  }
  for (const auto& [key, value] : model) {
    REQUIRE(map.get_copied(key) == std::optional<std::uint64_t>(value));
  }
}

TEST_CASE("remove_if removes only what its condition holds of", "[shared]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  std::map<std::uint64_t, std::uint64_t> model;
  for (std::uint64_t key = 0; key < 8'000; ++key) {
    map.insert(key, key * 3);
    model.emplace(key, key * 3);
  }
  std::mt19937_64 rng(13);
  for (int step = 0; step < 20'000; ++step) {
    const std::uint64_t key = rng() % 9'000;
    const bool odd_only = step % 2 == 0;
    const auto at = model.find(key);
    const bool removes = at != model.end() && (!odd_only || at->second % 2 == 1);
    const auto removed = map.remove_if(
        key, [&](const std::uint64_t&, const std::uint64_t& value) { return !odd_only || value % 2 == 1; });
    REQUIRE(removed.has_value() == removes);
    if (removes) {
      REQUIRE(*removed == std::pair<std::uint64_t, std::uint64_t>(key, at->second));
      model.erase(at);
    }
  }
  for (std::uint64_t key = 0; key < 9'000; ++key) {
    const auto at = model.find(key);
    REQUIRE(map.get_copied(key) ==
            (at == model.end() ? std::nullopt : std::optional<std::uint64_t>(at->second)));
  }
}

TEST_CASE("the owner extends a map with entries that only move, and changes them in place", "[shared]") {
  bbtree::bbtree_map<std::uint64_t, std::unique_ptr<std::uint64_t>> map;
  std::vector<std::pair<std::uint64_t, std::unique_ptr<std::uint64_t>>> entries;
  for (std::uint64_t key = 0; key < 3'000; ++key) {
    entries.emplace_back(key * 5 % 3'001, std::make_unique<std::uint64_t>(key));
  }
  map.extend(std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
  for (std::uint64_t key = 0; key < 3'000; ++key) {
    std::unique_ptr<std::uint64_t>* owned = map.get_owned(key * 5 % 3'001);
    REQUIRE(owned != nullptr);
    REQUIRE(**owned == key);
    **owned += 1;
  }
  REQUIRE(map.get_owned(std::uint64_t{3'001}) == nullptr);
  const std::optional<std::uint64_t> read =
      map.read(std::uint64_t{5},
               [](const std::uint64_t&, const std::unique_ptr<std::uint64_t>& value) { return *value; });
  REQUIRE(read == std::optional<std::uint64_t>(2));
}
