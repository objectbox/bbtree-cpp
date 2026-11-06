// The set's std::set interface and its set algebra, against std::set and the
// std algorithms; after the Rust crate's `unsync/set/tests.rs`.
#include <bbtree/unsync/set.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

static_assert(std::bidirectional_iterator<bbtree::unsync::bbtree_set<std::uint64_t>::const_iterator>);

template <class K>
K make_key(std::uint64_t n) {
  if constexpr (std::is_same_v<K, std::string>) {
    return "/srv/files/" + std::to_string(n % 31) + "/" + std::to_string(n);
  } else {
    return static_cast<K>(n);
  }
}

template <class Set, class Model>
void check_equal(const Set& set, const Model& model) {
  set.check_invariants();
  REQUIRE(set.size() == model.size());
  REQUIRE(std::equal(set.begin(), set.end(), model.begin(), model.end()));
}

}  // namespace

TEMPLATE_TEST_CASE("the set matches std::set", "[set]", std::uint64_t, std::string) {
  using K = TestType;
  bbtree::unsync::bbtree_set<K> set;
  std::set<K> model;
  std::mt19937_64 rng(3);
  for (int step = 0; step < 40'000; ++step) {
    const K key = make_key<K>(rng() % 5000);
    switch (rng() % 4) {
      case 0:
      case 1:
        REQUIRE(set.insert(key).second == model.insert(key).second);
        break;
      case 2:
        REQUIRE(set.erase(key) == model.erase(key));
        break;
      default: {
        const auto lower = set.lower_bound(key);
        const auto model_lower = model.lower_bound(key);
        REQUIRE((lower == set.end()) == (model_lower == model.end()));
        if (lower != set.end()) {
          REQUIRE(*lower == *model_lower);
        }
        REQUIRE(set.contains(key) == model.contains(key));
      }
    }
    if (step % 1000 == 0) {
      check_equal(set, model);
    }
  }
  check_equal(set, model);
}

TEST_CASE("set algebra matches the std algorithms", "[set]") {
  using set_type = bbtree::unsync::bbtree_set<std::uint64_t>;
  std::mt19937_64 rng(11);
  for (const auto& [a_count, b_count] : std::vector<std::pair<std::size_t, std::size_t>>{
           {0, 0}, {0, 100}, {100, 0}, {50, 5000}, {5000, 50}, {3000, 3000}, {20'000, 7}}) {
    std::set<std::uint64_t> a_model;
    std::set<std::uint64_t> b_model;
    for (std::size_t i = 0; i < a_count; ++i) {
      a_model.insert(rng() % 20'000);
    }
    for (std::size_t i = 0; i < b_count; ++i) {
      b_model.insert(rng() % 20'000);
    }
    const set_type a(a_model.begin(), a_model.end());
    const set_type b(b_model.begin(), b_model.end());
    std::vector<std::uint64_t> expected;
    std::set_union(a_model.begin(), a_model.end(), b_model.begin(), b_model.end(),
                   std::back_inserter(expected));
    check_equal(a | b, expected);
    expected.clear();
    std::set_intersection(a_model.begin(), a_model.end(), b_model.begin(), b_model.end(),
                          std::back_inserter(expected));
    check_equal(a & b, expected);
    expected.clear();
    std::set_difference(a_model.begin(), a_model.end(), b_model.begin(), b_model.end(),
                        std::back_inserter(expected));
    check_equal(a - b, expected);
    expected.clear();
    std::set_symmetric_difference(a_model.begin(), a_model.end(), b_model.begin(), b_model.end(),
                                  std::back_inserter(expected));
    check_equal(a ^ b, expected);
    CHECK(a.is_subset(b) == std::includes(b_model.begin(), b_model.end(), a_model.begin(), a_model.end()));
    CHECK((a & b).is_subset(a));
    CHECK(a.is_superset(a & b));
    CHECK(a.is_disjoint(b) == (a & b).empty());
    CHECK((a - b).is_disjoint(b));
  }
}

TEST_CASE("set bulk builds, splits and appends", "[set]") {
  using set_type = bbtree::unsync::bbtree_set<std::string>;
  std::vector<std::string> keys;
  for (int i = 0; i < 20'000; ++i) {
    keys.push_back(make_key<std::string>(static_cast<std::uint64_t>(i) * 7919 % 20'011));
  }
  set_type set(keys.begin(), keys.end());
  std::set<std::string> model(keys.begin(), keys.end());
  check_equal(set, model);
  const std::string cut = make_key<std::string>(9000);
  set_type upper = set.split_off(cut);
  std::set<std::string> model_upper(model.lower_bound(cut), model.end());
  model.erase(model.lower_bound(cut), model.end());
  check_equal(set, model);
  check_equal(upper, model_upper);
  set.append(upper);
  model.insert(model_upper.begin(), model_upper.end());
  check_equal(set, model);
  CHECK(upper.empty());
  CHECK(*set.first() == *model.begin());
  CHECK(set.pop_last() == *model.rbegin());
}

TEST_CASE("erasing by iterator walks the set", "[set]") {
  bbtree::unsync::bbtree_set<std::uint64_t> set;
  for (std::uint64_t i = 0; i < 10'000; ++i) {
    set.insert(i);
  }
  CHECK(erase_if(set, [](std::uint64_t key) { return key % 2 == 1; }) == 5000);
  auto it = set.begin();
  for (std::uint64_t expected = 0; it != set.end(); expected += 2, ++it) {
    REQUIRE(*it == expected);
  }
  set.check_invariants();
}
