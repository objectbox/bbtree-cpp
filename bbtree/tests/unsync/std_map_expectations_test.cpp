// What code written for std::map and std::set expects of a replacement, where
// the map's own interface code (no part of the Rust crate) decides: each
// case runs against the std container too, which says what is expected.
#include <bbtree/compact_string.hpp>
#include <bbtree/unsync/map.hpp>
#include <bbtree/unsync/set.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using u64_map = bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t>;
using u64_set = bbtree::unsync::bbtree_set<std::uint64_t>;

// The two maps under one shape, for cases that run on several entry types.
template <class K, class V>
using bbtree_map_of = bbtree::unsync::bbtree_map<K, V>;
template <class K, class V>
using std_map_of = std::map<K, V>;

// A value that can be moved and copied into place, never assigned.
struct labelled {
  const std::uint64_t id;
  std::string label;

  labelled(std::uint64_t id_, std::string label_) : id(id_), label(std::move(label_)) {}
  labelled(const labelled&) = default;
  labelled(labelled&&) noexcept = default;
};

// std::map asks assignment of its mapped type only where the user assigns
// (`insert_or_assign`, `map[key] = value`).
template <template <class, class> class MapOf>
struct values_that_cannot_be_assigned {
  static void run() {
    MapOf<std::uint64_t, labelled> map;
    REQUIRE(map.try_emplace(1, 1u, "one").second);
    REQUIRE(map.emplace(2, labelled(2, "two")).second);
    REQUIRE(map.insert({3, labelled(3, "three")}).second);
    REQUIRE_FALSE(map.try_emplace(1, 9u, "nine").second);
    REQUIRE_FALSE(map.emplace(2, labelled(9, "nine")).second);
    REQUIRE((*map.find(1)).second.label == "one");
    // Enough to split and merge nodes, which construct and destroy.
    for (std::uint64_t key = 4; key < 3000; ++key) {
      REQUIRE(map.try_emplace(key * 7919 % 3001 + 4, key, "a label long enough to live on the heap").second);
    }
    for (std::uint64_t key = 0; key < 3100; key += 2) {
      map.erase(key);
    }
    REQUIRE(map.at(3).label == "three");
    REQUIRE(map.find(4) == map.end());
    REQUIRE(map.size() > 1000);

    MapOf<std::uint64_t, const std::uint64_t> constant;
    REQUIRE(constant.try_emplace(1, 10).second);
    REQUIRE(constant.emplace(2, 20).second);
    REQUIRE_FALSE(constant.insert({2, 21}).second);
    REQUIRE(constant.at(2) == 20);
  }
};

}  // namespace

// Generic code erases `[lower_bound(a), lower_bound(b))` with no key between.
TEMPLATE_TEST_CASE("erasing an empty range erases nothing", "[map][std]", u64_map,
                   (std::map<std::uint64_t, std::uint64_t>)) {
  for (const std::uint64_t count : {10u, 5000u}) {
    TestType map;
    for (std::uint64_t key = 0; key < count; ++key) {
      map.insert({2 * key, key});
    }
    for (const std::uint64_t key : {std::uint64_t{0}, count, 2 * count - 2}) {
      const auto at = map.find(key);
      const auto next = map.erase(at, at);
      REQUIRE(map.size() == count);
      REQUIRE(next != map.end());
      REQUIRE((*next).first == key);
      const auto between = map.erase(map.lower_bound(key + 1), map.upper_bound(key + 1));
      REQUIRE(map.size() == count);
      REQUIRE(between == map.upper_bound(key));
    }
    REQUIRE(map.erase(map.end(), map.end()) == map.end());
    REQUIRE(map.size() == count);
  }
}

TEMPLATE_TEST_CASE("erasing an empty range of a set erases nothing", "[set][std]", u64_set,
                   std::set<std::uint64_t>) {
  TestType set;
  for (std::uint64_t key = 0; key < 5000; ++key) {
    set.insert(key);
  }
  const auto at = set.find(2500);
  const auto next = set.erase(at, at);
  REQUIRE(set.size() == 5000);
  REQUIRE(*next == 2500);
  REQUIRE(set.erase(set.end(), set.end()) == set.end());
}

TEST_CASE("a mapped type that can't be assigned can be inserted, found and erased", "[map][std]") {
  values_that_cannot_be_assigned<std_map_of>::run();
  values_that_cannot_be_assigned<bbtree_map_of>::run();
}

