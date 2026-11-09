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

