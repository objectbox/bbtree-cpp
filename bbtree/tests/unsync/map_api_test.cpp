// The map's std::map interface: lookups, inserts, erases, iterators, bounds
// and comparisons, after the Rust crate's `unsync/map/tests`.
#include <bbtree/unsync/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <vector>

#include "../support/test_types.hpp"

namespace {

using u64_map = bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t>;
using string_map = bbtree::unsync::bbtree_map<std::string, std::string, std::less<>>;

static_assert(std::bidirectional_iterator<u64_map::iterator>);
static_assert(std::bidirectional_iterator<u64_map::const_iterator>);
static_assert(std::bidirectional_iterator<string_map::iterator>);
static_assert(std::ranges::bidirectional_range<u64_map>);
static_assert(sizeof(u64_map) == 64, "the map is a cache line (D50, D52)");

}  // namespace

TEST_CASE("an empty map has no entries and allocates nothing", "[map]") {
  const u64_map map;
  CHECK(map.empty());
  CHECK(map.size() == 0);
  CHECK(map.begin() == map.end());
  CHECK(map.find(1) == map.end());
  CHECK_FALSE(map.first());
  map.check_invariants();
}

TEST_CASE("insert, find and the subscript operator", "[map]") {
  u64_map map;
  CHECK(map.insert({1, 10}).second);
  CHECK_FALSE(map.insert({1, 11}).second);
  CHECK(map.at(1) == 10);
  map[2] = 20;
  map[1] += 1;
  CHECK(map.at(1) == 11);
  CHECK(map.size() == 2);
  CHECK(map.contains(2));
  CHECK(map.count(3) == 0);
  CHECK_THROWS_AS(map.at(3), std::out_of_range);
  const auto it = map.find(2);
  REQUIRE(it != map.end());
  CHECK(it->first == 2);
  CHECK((*it).second == 20);
  it->second = 21;
  CHECK(map.at(2) == 21);
  map.check_invariants();
}

TEST_CASE("try_emplace leaves the key alone when it is there", "[map]") {
  bbtree::unsync::bbtree_map<std::string, std::string> map;
  std::string key = "a key long enough to live on the heap";
  std::string value = "a value long enough to live on the heap";
  CHECK(map.try_emplace(std::move(key), std::move(value)).second);
  std::string again = "a key long enough to live on the heap";
  CHECK_FALSE(map.try_emplace(std::move(again), "other").second);
  CHECK(again == "a key long enough to live on the heap");
  CHECK(map.begin()->second == "a value long enough to live on the heap");
}

TEST_CASE("insert_or_assign replaces values", "[map]") {
  u64_map map;
  CHECK(map.insert_or_assign(5, 50).second);
  CHECK_FALSE(map.insert_or_assign(5, 51).second);
  CHECK(map.at(5) == 51);
}

TEST_CASE("iteration visits every entry in order, both ways", "[map]") {
  u64_map map;
  std::map<std::uint64_t, std::uint64_t> model;
  for (std::uint64_t i = 0; i < 5000; ++i) {
    const std::uint64_t key = i * 7919 % 10007;
    map.insert_or_assign(key, i);
    model.insert_or_assign(key, i);
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> forward;
  for (const auto& [key, value] : map) {
    forward.emplace_back(key, value);
  }
  CHECK(forward == std::vector<std::pair<std::uint64_t, std::uint64_t>>(model.begin(), model.end()));
  std::vector<std::pair<std::uint64_t, std::uint64_t>> backward;
  for (auto it = map.rbegin(); it != map.rend(); ++it) {
    backward.emplace_back(it->first, it->second);
  }
  CHECK(backward == std::vector<std::pair<std::uint64_t, std::uint64_t>>(model.rbegin(), model.rend()));
  CHECK(static_cast<std::size_t>(std::distance(map.begin(), map.end())) == map.size());
}

TEST_CASE("an iterator from find steps on through the tree", "[map]") {
  u64_map map;
  for (std::uint64_t key = 0; key < 4000; ++key) {
    map.insert({key * 2, key});
  }
  for (std::uint64_t key = 0; key < 8000; key += 314) {
    auto it = map.find(key);
    REQUIRE(it != map.end());
    std::uint64_t expected = key;
    for (int step = 0; step < 200 && it != map.end(); ++step, ++it) {
      REQUIRE(it->first == expected);
      expected += 2;
    }
    auto back = map.find(key);
    for (std::uint64_t expected_back = key; expected_back > 0 && expected_back + 400 > key;
         expected_back -= 2) {
      REQUIRE(back->first == expected_back);
      --back;
    }
  }
}

TEST_CASE("bounds find the first entry not below and above a key", "[map]") {
  u64_map map;
  std::map<std::uint64_t, std::uint64_t> model;
  for (std::uint64_t i = 0; i < 3000; ++i) {
    map.insert({i * 3, i});
    model.insert({i * 3, i});
  }
  for (std::uint64_t key = 0; key < 9010; key += 7) {
    const auto lower = map.lower_bound(key);
    const auto model_lower = model.lower_bound(key);
    REQUIRE((lower == map.end()) == (model_lower == model.end()));
    if (model_lower != model.end()) {
      REQUIRE(lower->first == model_lower->first);
    }
    const auto upper = map.upper_bound(key);
    const auto model_upper = model.upper_bound(key);
    REQUIRE((upper == map.end()) == (model_upper == model.end()));
    if (model_upper != model.end()) {
      REQUIRE(upper->first == model_upper->first);
    }
    REQUIRE(std::distance(lower, upper) == std::distance(model_lower, model_upper));
  }
}

TEST_CASE("erase by key and by iterator returns the next entry", "[map]") {
  u64_map map;
  std::map<std::uint64_t, std::uint64_t> model;
  for (std::uint64_t i = 0; i < 6000; ++i) {
    map.insert({i, i});
    model.insert({i, i});
  }
  CHECK(map.erase(17) == 1);
  CHECK(map.erase(17) == 0);
  model.erase(17);
  auto it = map.begin();
  auto model_it = model.begin();
  int step = 0;
  while (it != map.end()) {
    REQUIRE(it->first == model_it->first);
    if (step++ % 3 == 0) {
      it = map.erase(it);
      model_it = model.erase(model_it);
    } else {
      ++it;
      ++model_it;
    }
    if (step % 500 == 0) {
      map.check_invariants();
    }
  }
  CHECK(model_it == model.end());
  CHECK(map.size() == model.size());
  map.check_invariants();
  CHECK(std::equal(map.begin(), map.end(), model.begin(), model.end(),
                   [](auto a, const auto& b) { return a.first == b.first && a.second == b.second; }));
}

TEST_CASE("erase of a range", "[map]") {
  u64_map map;
  for (std::uint64_t i = 0; i < 2000; ++i) {
    map.insert({i, i});
  }
  auto next = map.erase(map.find(100), map.find(1500));
  REQUIRE(next != map.end());
  CHECK(next->first == 1500);
  CHECK(map.size() == 600);
  map.erase(map.find(1900), map.end());
  CHECK(map.size() == 500);
  map.check_invariants();
}

TEST_CASE("erase_if and retain", "[map]") {
  u64_map map;
  for (std::uint64_t i = 0; i < 3000; ++i) {
    map.insert({i, i});
  }
  CHECK(erase_if(map, [](auto entry) { return entry.first % 3 == 0; }) == 1000);
  CHECK(map.size() == 2000);
  for (const auto& [key, value] : map) {
    REQUIRE(key % 3 != 0);
  }
  map.check_invariants();
}

TEST_CASE("transparent lookups take string views", "[map]") {
  string_map map;
  map.insert({"apple", "red"});
  map.insert({"banana", "yellow"});
  const std::string_view key = "banana";
  REQUIRE(map.find(key) != map.end());
  CHECK(map.find(key)->second == "yellow");
  CHECK(map.contains("apple"));
  CHECK(map.erase(std::string_view("apple")) == 1);
  CHECK_FALSE(map.contains("apple"));
}

TEST_CASE("copies, moves, swaps and comparisons", "[map]") {
  u64_map map{{1, 1}, {2, 4}, {3, 9}};
  u64_map copy = map;
  CHECK(copy == map);
  copy[4] = 16;
  CHECK(copy != map);
  CHECK(map < copy);
  u64_map moved = std::move(copy);
  CHECK(moved.size() == 4);
  CHECK(copy.empty());
  swap(moved, map);
  CHECK(map.size() == 4);
  CHECK(moved.size() == 3);
  map.check_invariants();
  moved.check_invariants();
}

TEST_CASE("pop_first, pop_last, first and last", "[map]") {
  u64_map map;
  for (std::uint64_t i = 10; i < 20; ++i) {
    map.insert({i, i * i});
  }
  CHECK(map.first()->first == 10);
  CHECK(map.last()->second == 19 * 19);
  CHECK(map.pop_first()->first == 10);
  CHECK(map.pop_last()->first == 19);
  CHECK(map.size() == 8);
}

TEST_CASE("append and split_off", "[map]") {
  u64_map left;
  u64_map right;
  for (std::uint64_t i = 0; i < 1000; ++i) {
    left.insert({i, i});
    right.insert({i + 500, i + 1});
  }
  left.append(right);
  CHECK(right.empty());
  CHECK(left.size() == 1500);
  CHECK(left.at(600) == 101);
  u64_map upper = left.split_off(700);
  CHECK(left.size() == 700);
  CHECK(upper.size() == 800);
  CHECK(upper.begin()->first == 700);
  left.check_invariants();
  upper.check_invariants();
}

TEST_CASE("every tracked value is destroyed once", "[map]") {
  bbtree_test::drop_ledger ledger;
  {
    bbtree::unsync::bbtree_map<std::uint64_t, bbtree_test::tracked> map;
    for (std::uint64_t i = 0; i < 5000; ++i) {
      map.try_emplace(i * 31 % 5003, ledger, i);
    }
    for (std::uint64_t i = 0; i < 5000; i += 3) {
      map.erase(i);
    }
    auto copy = map;
    copy.clear();
    map.check_invariants();
  }
  CHECK(ledger.alive() == 0);
}
