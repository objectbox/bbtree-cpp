// Drop accounting for the set, after the Rust crate's `unsync/set/tests.rs`:
// its own entry points onto the map's tree hold and hand back the very key
// objects they were given, and the operations on two sets, which copy keys
// into bulk builds, leave their operands' objects alone.
#include "../support/tracked_maps.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <vector>

namespace {

using bbtree_test::checked_allocator;
using bbtree_test::object_ledger;
using bbtree_test::page_book;

// A set of tracked keys beside a model of which key object it holds at each
// order.
template <class Set>
class mirrored_set {
  using K = typename Set::key_type;

 public:
  mirrored_set(object_ledger& ledger, page_book& pages) : ledger_(&ledger), set_(checked_allocator(pages)) {}

  Set& set() noexcept { return set_; }
  std::size_t size() const noexcept { return model_.size(); }

  void require_same() const {
    set_.check_invariants();
    REQUIRE(set_.size() == model_.size());
    auto expected = model_.begin();
    for (const K& key : set_) {
      REQUIRE(key.order() == expected->first);
      REQUIRE(key.id() == expected->second);
      ++expected;
    }
    auto expected_back = model_.rbegin();
    for (auto at = set_.rbegin(); at != set_.rend(); ++at, ++expected_back) {
      REQUIRE(at->id() == expected_back->second);
    }
  }

  // The set keeps the key it has: the argument is left alone.
  void insert_moved(std::uint64_t order) {
    K key(*ledger_, order);
    const object_ledger::object_id id = key.id();
    const auto [at, inserted] = set_.insert(std::move(key));
    REQUIRE(inserted == model_.emplace(order, id).second);
    REQUIRE(key.moved_from() == inserted);
    REQUIRE(at->id() == model_.at(order));
  }
  void insert_copied(std::uint64_t order) {
    const K key(*ledger_, order);
    const auto [at, inserted] = set_.insert(key);
    REQUIRE(inserted == !model_.contains(order));
    REQUIRE(at->id() == model_.emplace(order, at->id()).first->second);
  }
  void emplace(std::uint64_t order) {
    const auto [at, inserted] = set_.emplace(*ledger_, order);
    REQUIRE(inserted == !model_.contains(order));
    REQUIRE(at->id() == model_.emplace(order, at->id()).first->second);
  }
  void erase_key(std::uint64_t order) { REQUIRE(set_.erase(K(*ledger_, order)) == model_.erase(order)); }
  void erase_at(std::uint64_t order) {
    const auto at = set_.find(K(*ledger_, order));
    REQUIRE((at == set_.end()) == !model_.contains(order));
    if (at != set_.end()) {
      const auto next = set_.erase(at);
      const auto expected_next = model_.erase(model_.find(order));
      REQUIRE((next == set_.end()) == (expected_next == model_.end()));
      REQUIRE((next == set_.end() || next->id() == expected_next->second));
    }
  }
  void take(std::uint64_t order) {
    const std::optional<K> taken = set_.take(K(*ledger_, order));
    REQUIRE(taken.has_value() == model_.contains(order));
    if (taken) {
      REQUIRE(taken->id() == model_.at(order));
      model_.erase(order);
    }
  }
  void pop(bool last) {
    const std::optional<K> popped = last ? set_.pop_last() : set_.pop_first();
    REQUIRE(popped.has_value() == !model_.empty());
    if (popped) {
      const auto expected = last ? std::prev(model_.end()) : model_.begin();
      REQUIRE(popped->id() == expected->second);
      model_.erase(expected);
    }
  }
  void look_up(std::uint64_t order) const {
    const K probe(*ledger_, order);
    REQUIRE(set_.contains(probe) == model_.contains(order));
    REQUIRE(set_.count(probe) == model_.count(order));
  }

 private:
  object_ledger* ledger_;
  Set set_;
  std::map<std::uint64_t, object_ledger::object_id> model_;
};

// Two sets by where their keys lie: `count` keys from `from`, `step` apart.
struct key_run {
  std::uint64_t from;
  std::uint64_t count;
  std::uint64_t step;

  template <class Set>
  Set build(object_ledger& ledger, page_book& pages) const {
    Set built{checked_allocator(pages)};
    for (std::uint64_t index = 0; index < count; ++index) {
      built.emplace(ledger, from + index * step);
    }
    return built;
  }
};

template <class Set>
std::vector<std::uint64_t> orders_of(const Set& set) {
  std::vector<std::uint64_t> orders;
  for (const auto& key : set) {
    orders.push_back(key.order());
  }
  return orders;
}

}  // namespace

// set/tests.rs: u64_set_matches_std, which_of_equal_elements_stays_matches_std.
TEMPLATE_TEST_CASE("the set holds and hands back the key objects it was given", "[set][drops]",
                   BBTREE_TEST_TRACKED_SETS) {
  object_ledger ledger;
  page_book pages;
  {
    mirrored_set<TestType> sets(ledger, pages);
    std::mt19937_64 rng(37);
    for (int step = 0; step < 20'000; ++step) {
      const std::uint64_t order = rng() % 3000;
      switch (rng() % 10) {
        case 0:
        case 1:
        case 2:
          sets.insert_moved(order);
          break;
        case 3:
          sets.insert_copied(order);
          break;
        case 4:
          sets.emplace(order);
          break;
        case 5:
          sets.erase_key(order);
          break;
        case 6:
          sets.take(order);
          break;
        case 7:
          sets.pop(rng() % 2 == 0);
          break;
        case 8:
          sets.erase_at(order);
          break;
        default:
          sets.look_up(order);
          break;
      }
      if (step % 2048 == 0) {
        sets.require_same();
      }
    }
    sets.require_same();
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// set/tests.rs: set_operations_match_std. Pairs apart, touching, nested and
// interleaved, of lengths on both sides of where looking keys up beats
// walking both sets (16 times as many keys).
TEMPLATE_TEST_CASE("set algebra copies keys and leaves its operands alone", "[set][drops]",
                   bbtree_test::singly_moved_plain_key_set, bbtree_test::byte_moved_narrow_inline_key_set) {
  using K = typename TestType::key_type;
  object_ledger ledger;
  page_book pages;
  page_book other_pages;
  const std::pair<key_run, key_run> shapes[] = {
      {{0, 0, 1}, {0, 0, 1}},       {{0, 0, 1}, {0, 100, 1}},     {{0, 100, 1}, {0, 0, 1}},
      {{0, 500, 2}, {1, 500, 2}},   {{0, 500, 1}, {500, 500, 1}}, {{0, 500, 1}, {499, 500, 1}},
      {{0, 2000, 1}, {700, 20, 3}}, {{700, 20, 3}, {0, 2000, 1}}, {{0, 1600, 1}, {0, 100, 16}},
      {{0, 1601, 1}, {0, 100, 16}}, {{0, 100, 16}, {0, 1700, 1}}, {{0, 3000, 2}, {0, 3000, 3}}};
  for (const auto& [run_a, run_b] : shapes) {
    const TestType a = run_a.build<TestType>(ledger, pages);
    const TestType b = run_b.build<TestType>(ledger, other_pages);
    const std::vector<std::uint64_t> in_a = orders_of(a);
    const std::vector<std::uint64_t> in_b = orders_of(b);
    const long live_before = ledger.live();
    {
      std::vector<std::uint64_t> expected;
      const TestType united = a | b;
      std::set_union(in_a.begin(), in_a.end(), in_b.begin(), in_b.end(), std::back_inserter(expected));
      united.check_invariants();
      REQUIRE(orders_of(united) == expected);
      expected.clear();
      const TestType common = a & b;
      std::set_intersection(in_a.begin(), in_a.end(), in_b.begin(), in_b.end(), std::back_inserter(expected));
      common.check_invariants();
      REQUIRE(orders_of(common) == expected);
      expected.clear();
      const TestType only_a = a - b;
      std::set_difference(in_a.begin(), in_a.end(), in_b.begin(), in_b.end(), std::back_inserter(expected));
      only_a.check_invariants();
      REQUIRE(orders_of(only_a) == expected);
      expected.clear();
      const TestType either = a ^ b;
      std::set_symmetric_difference(in_a.begin(), in_a.end(), in_b.begin(), in_b.end(),
                                    std::back_inserter(expected));
      either.check_invariants();
      REQUIRE(orders_of(either) == expected);
      REQUIRE(a.is_subset(b) == std::includes(in_b.begin(), in_b.end(), in_a.begin(), in_a.end()));
      REQUIRE(a.is_superset(b) == std::includes(in_a.begin(), in_a.end(), in_b.begin(), in_b.end()));
      REQUIRE(common.is_subset(a));
      REQUIRE(common.is_subset(b));
      REQUIRE(a.is_disjoint(b) == common.empty());
      REQUIRE(only_a.is_disjoint(b));
      // Each result holds copies: its operands' objects are all still theirs.
      REQUIRE(ledger.live() ==
              live_before + static_cast<long>(united.size() + common.size() + only_a.size() + either.size()));
    }
    REQUIRE(ledger.live() == live_before);
    // Cuts, joins and bulk builds through the set's own entry points.
    TestType copy(a);
    TestType tail = copy.split_off(K(ledger, run_a.from + run_a.count * run_a.step / 2));
    copy.check_invariants();
    tail.check_invariants();
    REQUIRE(copy.size() + tail.size() == a.size());
    copy.append(tail);
    REQUIRE(copy == a);
    std::vector<K> keys_of_b;
    for (const std::uint64_t order : in_b) {
      keys_of_b.emplace_back(ledger, order);
    }
    TestType from_range(keys_of_b.rbegin(), keys_of_b.rend(),
                        checked_allocator(pages));  // descending: sorted first
    REQUIRE(orders_of(from_range) == in_b);
    const auto odd = [](std::uint64_t order) { return order % 2 != 0; };
    REQUIRE(from_range.retain([&](const K& key) { return !odd(key.order()); }) ==
            static_cast<std::size_t>(std::count_if(in_b.begin(), in_b.end(), odd)));
    from_range.check_invariants();
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(other_pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

TEMPLATE_TEST_CASE("set batches insert and look up as the single operations do", "[set][drops][batch]",
                   bbtree_test::singly_moved_inline_key_set, bbtree_test::byte_moved_plain_key_set) {
  using K = typename TestType::key_type;
  object_ledger ledger;
  page_book pages;
  {
    TestType set{checked_allocator(pages)};
    std::mt19937_64 rng(39);
    std::set<std::uint64_t> model;
    for (int round = 0; round < 40; ++round) {
      std::array<K, 100> keys = [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<K, 100>{((void)I, K(ledger, rng() % 4000))...};
      }(std::make_index_sequence<100>());
      std::array<bool, 100> expected_new;
      for (std::size_t index = 0; index < keys.size(); ++index) {
        expected_new[index] = model.insert(keys[index].order()).second;
      }
      REQUIRE(set.insert_many(std::move(keys)) == expected_new);
    }
    set.check_invariants();
    REQUIRE(orders_of(set) == std::vector<std::uint64_t>(model.begin(), model.end()));
    const std::array<K, 3> probes{K(ledger, *model.begin()), K(ledger, 999'999), K(ledger, *model.rbegin())};
    REQUIRE((set.contains_many(probes) == std::array<bool, 3>{true, false, true}));
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}
