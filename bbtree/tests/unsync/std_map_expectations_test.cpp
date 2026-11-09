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

// Random inserts, lookups, bounds and erases of keys `K`, made by `key_of`
// from a number, on a map and a std::map; then both hold the same.
template <class K>
struct against_std_map {
  template <class KeyOf>
  static void run(KeyOf key_of) {
    bbtree::unsync::bbtree_map<K, std::uint64_t> map;
    std::map<K, std::uint64_t> model;
    std::mt19937_64 rng(11);
    for (std::uint64_t step = 0; step < 20'000; ++step) {
      const K key = key_of(rng() % 5000);
      switch (rng() % 4) {
        case 0:
        case 1:
          REQUIRE(map.insert_or_assign(key, step).second == model.insert_or_assign(key, step).second);
          break;
        case 2:
          REQUIRE(map.erase(key) == model.erase(key));
          break;
        default: {
          REQUIRE(map.contains(key) == model.contains(key));
          const auto lower = map.lower_bound(key);
          const auto expected = model.lower_bound(key);
          REQUIRE((lower == map.end()) == (expected == model.end()));
          REQUIRE((lower == map.end() || lower->first == expected->first));
          break;
        }
      }
    }
    map.check_invariants();
    REQUIRE(map.size() == model.size());
    REQUIRE(std::equal(map.begin(), map.end(), model.begin(), model.end(),
                       [](auto a, const auto& b) { return a.first == b.first && a.second == b.second; }));
  }
};

// Three words whose addresses ascend and whose order descends: compared as
// what they are, pointers, a range of them looks sorted.
struct words_in_one_array {
  static constexpr char storage[] = "pear\0fig\0apple";
  static constexpr const char* pear = storage;
  static constexpr const char* fig = storage + 5;
  static constexpr const char* apple = storage + 9;
};

// A key made of a number only on request.
struct explicit_number {
  std::uint64_t number;
  explicit explicit_number(std::uint64_t number_) : number(number_) {}
  friend auto operator<=>(const explicit_number&, const explicit_number&) = default;
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

// `double <=> double` is a `std::partial_ordering`; std::map takes such keys
// (and leaves a NaN key to the user).
TEST_CASE("keys whose three-way comparison is a partial order", "[map][std]") {
  against_std_map<double>::run(
      [](std::uint64_t number) { return static_cast<double>(number) / 8.0 - 300.0; });
  against_std_map<float>::run([](std::uint64_t number) { return static_cast<float>(number) * 0.5f; });
  against_std_map<std::pair<int, double>>::run([](std::uint64_t number) {
    return std::pair<int, double>(static_cast<int>(number % 7), static_cast<double>(number / 7));
  });
}

// `std::less<>` is what README recommends for string keys.
TEST_CASE("a range of elements that convert to keys builds a sorted map", "[map][set][std][build]") {
  const std::vector<std::pair<const char*, int>> entries{
      {words_in_one_array::pear, 1}, {words_in_one_array::fig, 2}, {words_in_one_array::apple, 3}};
  const std::map<std::string, int, std::less<>> model(entries.begin(), entries.end());
  SECTION("C strings into a map of std::string") {
    const bbtree::unsync::bbtree_map<std::string, int, std::less<>> map(entries.begin(), entries.end());
    map.check_invariants();
    REQUIRE(std::equal(map.begin(), map.end(), model.begin(), model.end(),
                       [](auto a, const auto& b) { return a.first == b.first && a.second == b.second; }));
    REQUIRE(map.contains("apple"));
    REQUIRE(map.contains(std::string_view("pear")));
  }
  SECTION("C strings into a map of compact_string") {
    const bbtree::unsync::bbtree_map<bbtree::compact_string, int, std::less<>> map(entries.begin(),
                                                                                   entries.end());
    map.check_invariants();
    REQUIRE(map.size() == 3);
    REQUIRE(std::string_view(map.begin()->first) == "apple");
    REQUIRE(map.contains("fig"));
  }
  SECTION("C strings into a set of std::string") {
    const std::vector<const char*> keys{words_in_one_array::pear, words_in_one_array::fig,
                                        words_in_one_array::apple};
    const bbtree::unsync::bbtree_set<std::string, std::less<>> set(keys.begin(), keys.end());
    set.check_invariants();
    REQUIRE(std::vector<std::string>(set.begin(), set.end()) ==
            std::vector<std::string>{"apple", "fig", "pear"});
    REQUIRE(set.contains("apple"));
  }
  SECTION("numbers into a set of keys made of them on request, as std::set takes them") {
    const std::vector<std::uint64_t> numbers{3, 1, 2, 1};
    const std::set<explicit_number> expected(numbers.begin(), numbers.end());
    const bbtree::unsync::bbtree_set<explicit_number> set(numbers.begin(), numbers.end());
    set.check_invariants();
    REQUIRE(std::equal(set.begin(), set.end(), expected.begin(), expected.end()));
    const std::vector<std::uint64_t> ascending{5, 6, 7};
    const bbtree::unsync::bbtree_set<explicit_number> built(ascending.begin(), ascending.end());
    REQUIRE(built.size() == 3);
  }
}

