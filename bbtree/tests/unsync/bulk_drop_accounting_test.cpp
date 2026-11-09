// Drop accounting for the operations on whole maps, after the Rust crate's
// `unsync/map/tests/drops.rs`, `allocator.rs` and `batch.rs`: bulk builds,
// copies and moves, appends, cuts and batches destroy every key and value
// once, move the objects they were given where a move is promised, and give
// every page back to the allocator it came from.
#include "../support/mirrored_map.hpp"
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
#include <span>
#include <utility>
#include <vector>

namespace {

using bbtree_test::checked_allocator;
using bbtree_test::entry_ids;
using bbtree_test::held_entries;
using bbtree_test::mirrored_map;
using bbtree_test::object_ledger;
using bbtree_test::page_book;

// A map of tracked entries whose orders run from `from` to `to` in steps of
// `step`, each with the payload `order + payload_base`.
template <class Map>
struct run_of_entries {
  std::uint64_t from;
  std::uint64_t to;
  std::uint64_t step;
  std::uint64_t payload_base;

  void fill(Map& map, object_ledger& ledger) const {
    for (std::uint64_t order = from; order < to; order += step) {
      map.try_emplace(typename Map::key_type(ledger, order), ledger, payload_base + order);
    }
  }

  void require_in(const Map& map) const {
    map.check_invariants();
    std::uint64_t order = from;
    for (const auto& [key, value] : map) {
      REQUIRE(order < to);
      REQUIRE(key.order() == order);
      REQUIRE(value.payload() == payload_base + order);
      order += step;
    }
    REQUIRE(order >= to);
  }
};

}  // namespace

// drops.rs: collect_keeps_the_last_of_equal_keys_like_std; in C++ a range
// keeps the first of equal keys, as std::map's constructor does.
TEMPLATE_TEST_CASE("bulk builds copy or move each entry once and keep the first of equal keys",
                   "[map][drops]", BBTREE_TEST_COPYABLE_TRACKED_MAPS) {
  using K = typename TestType::key_type;
  using V = typename TestType::mapped_type;
  object_ledger ledger;
  page_book pages;
  const checked_allocator allocator(pages);
  std::mt19937_64 rng(26);
  for (const std::size_t count : {0u, 1u, 2u, 11u, 12u, 13u, 14u, 15u, 200u, 3000u}) {
    // (order, payload) of each entry that goes in; the payloads number them.
    using input = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    input shuffled;
    input ascending;
    for (std::size_t index = 0; index < count; ++index) {
      shuffled.emplace_back(rng() % (count / 2 + 1), index);
      ascending.emplace_back(3 * index, index);
    }
    input sorted_with_equal_keys = shuffled;
    std::stable_sort(sorted_with_equal_keys.begin(), sorted_with_equal_keys.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const input* orders : {&shuffled, &sorted_with_equal_keys, &ascending}) {
      std::vector<std::pair<K, V>> entries;
      entries.reserve(count);
      std::map<std::uint64_t, std::uint64_t> first_payloads;
      std::map<std::uint64_t, entry_ids> first_objects;
      for (const auto& [order, payload] : *orders) {
        entries.emplace_back(K(ledger, order), V(ledger, payload));
        first_payloads.emplace(order, payload);
        first_objects.emplace(order, entry_ids{entries.back().first.id(), entries.back().second.id()});
      }
      const long live_before = ledger.live();
      {
        // Copied in: the input keeps its objects.
        const TestType copied(entries.begin(), entries.end(), allocator);
        copied.check_invariants();
        REQUIRE(copied.size() == first_payloads.size());
        auto expected = first_payloads.begin();
        for (const auto& [key, value] : copied) {
          REQUIRE(key.order() == expected->first);
          REQUIRE(value.payload() == expected->second);
          ++expected;
        }
        REQUIRE(ledger.live() == live_before + 2 * static_cast<long>(copied.size()));
        REQUIRE(std::none_of(entries.begin(), entries.end(), [](const auto& entry) {
          return entry.first.moved_from() || entry.second.moved_from();
        }));
      }
      REQUIRE(ledger.live() == live_before);
      REQUIRE(pages.in_use() == 0);
      {
        // Into a map with entries: one by one, existing keys kept.
        TestType map(allocator);
        map.try_emplace(K(ledger, 0), ledger, std::uint64_t{424242});
        map.insert(entries.begin(), entries.end());
        map.check_invariants();
        REQUIRE(map.begin()->second.payload() == 424242);
        REQUIRE(map.size() == first_payloads.size() + (first_payloads.contains(0) ? 0u : 1u));
      }
      REQUIRE(ledger.live() == live_before);
      {
        // Moved in: the map holds the very objects of the first of equal keys.
        const TestType moved(std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()),
                             allocator);
        moved.check_invariants();
        REQUIRE(held_entries<TestType>::forward(moved) ==
                typename held_entries<TestType>::entries(first_objects.begin(), first_objects.end()));
        entries.clear();
        REQUIRE(ledger.live() == 2 * static_cast<long>(moved.size()));
      }
      REQUIRE(ledger.nothing_left());
      REQUIRE(pages.in_use() == 0);
    }
  }
  {
    // An initializer list, assigned over a map with entries.
    TestType map(allocator);
    run_of_entries<TestType>{0, 500, 1, 0}.fill(map, ledger);
    map = {{K(ledger, 3), V(ledger, 30)}, {K(ledger, 1), V(ledger, 10)}, {K(ledger, 3), V(ledger, 31)}};
    map.check_invariants();
    REQUIRE(map.size() == 2);
    REQUIRE(map.begin()->second.payload() == 10);
    REQUIRE(std::next(map.begin())->second.payload() == 30);
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// The C++ analogue of Rust's moves: move construction and assignment, swap,
// the moved-from state and clear, across two allocators.
TEMPLATE_TEST_CASE("moves and swaps take the tree with its allocator", "[map][drops]",
                   BBTREE_TEST_TRACKED_MAPS) {
  using K = typename TestType::key_type;
  object_ledger ledger;
  page_book pages;
  page_book other_pages;
  {
    const run_of_entries<TestType> large{0, 1500, 1, 100};
    TestType a{checked_allocator(pages)};
    large.fill(a, ledger);
    const auto held = held_entries<TestType>::forward(a);
    const std::size_t pages_of_a = pages.in_use();

    TestType b(std::move(a));  // a move takes the tree: the same objects, the same pages
    REQUIRE(held_entries<TestType>::forward(b) == held);
    REQUIRE(pages.in_use() == pages_of_a);
    REQUIRE(a.empty());  // NOLINT: the moved-from map is empty and usable
    REQUIRE(a.begin() == a.end());
    REQUIRE(a.find(K(ledger, 1)) == a.end());
    a.check_invariants();
    const run_of_entries<TestType> small{0, 40, 1, 7};
    small.fill(a, ledger);
    small.require_in(a);

    TestType c{checked_allocator(other_pages)};
    run_of_entries<TestType>{0, 900, 3, 55}.fill(c, ledger);
    c = std::move(b);  // move assignment over entries: theirs are destroyed, their pages go back
    REQUIRE(other_pages.in_use() == 0);
    REQUIRE(held_entries<TestType>::forward(c) == held);
    REQUIRE(b.empty());  // NOLINT
    b.check_invariants();
    small.fill(b, ledger);
    small.require_in(b);

    TestType& same_c = c;
    c = std::move(same_c);  // self move assignment
    REQUIRE(held_entries<TestType>::forward(c) == held);

    const run_of_entries<TestType> sparse{0, 300, 2, 9};
    TestType d{checked_allocator(other_pages)};
    sparse.fill(d, ledger);
    swap(c, d);  // allocators go with their trees
    REQUIRE(held_entries<TestType>::forward(d) == held);
    sparse.require_in(c);
    c.clear();
    REQUIRE(other_pages.in_use() == 0);
    REQUIRE(c.empty());
    c.check_invariants();
    sparse.fill(c, ledger);
    REQUIRE(other_pages.in_use() > 0);
    c.clear();
    c.clear();
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(other_pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// The C++ analogue of Rust's clones.
TEMPLATE_TEST_CASE("copies have the same shape and share nothing", "[map][drops]",
                   BBTREE_TEST_COPYABLE_TRACKED_MAPS) {
  using K = typename TestType::key_type;
  object_ledger ledger;
  page_book pages;
  page_book other_pages;
  {
    const run_of_entries<TestType> large{0, 1500, 1, 100};
    TestType a{checked_allocator(pages)};
    large.fill(a, ledger);
    const auto held = held_entries<TestType>::forward(a);
    const long live_in_a = ledger.live();
    const std::size_t pages_of_a = pages.in_use();

    TestType b(a);  // the same entries in other objects, on the same allocator
    REQUIRE(ledger.live() == 2 * live_in_a);
    REQUIRE(pages.in_use() == 2 * pages_of_a);
    REQUIRE(b == a);
    large.require_in(b);
    const auto held_by_copy = held_entries<TestType>::forward(b);
    for (std::size_t index = 0; index < held.size(); ++index) {
      REQUIRE(held_by_copy[index].second.key != held[index].second.key);
      REQUIRE(held_by_copy[index].second.value != held[index].second.value);
    }
    b.erase(K(ledger, 700));
    b.try_emplace(K(ledger, 5000), ledger, std::uint64_t{1});
    REQUIRE(held_entries<TestType>::forward(a) == held);

    TestType c{checked_allocator(other_pages)};
    run_of_entries<TestType>{0, 900, 3, 55}.fill(c, ledger);
    c = a;  // copy assignment over entries: the copy comes with a's allocator
    REQUIRE(other_pages.in_use() == 0);
    large.require_in(c);
    REQUIRE(held_entries<TestType>::forward(a) == held);

    const TestType& same_c = c;
    c = same_c;  // self copy assignment
    large.require_in(c);
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(other_pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// drops.rs: append_drops_like_std; allocator.rs:
// append_keeps_each_page_with_its_own_allocator.
TEMPLATE_TEST_CASE("append moves the other map's entries in, its key and value winning", "[map][drops]",
                   BBTREE_TEST_TRACKED_MAPS) {
  using K = typename TestType::key_type;
  object_ledger ledger;
  page_book ours;
  page_book theirs;
  std::mt19937_64 rng(29);
  const std::pair<std::uint64_t, std::uint64_t> sizes[] = {{0, 0},   {0, 50},   {50, 0},   {1, 1},
                                                           {12, 13}, {1500, 3}, {3, 1500}, {2000, 2000}};
  for (const auto& [left_count, right_count] : sizes) {
    mirrored_map<TestType> left(ledger, ours);
    mirrored_map<TestType> right(ledger, theirs);
    const std::uint64_t key_space = left_count + right_count + 1;
    for (std::uint64_t index = 0; index < left_count; ++index) {
      left.try_emplace_moved(rng() % key_space, index);
    }
    for (std::uint64_t index = 0; index < right_count; ++index) {
      right.try_emplace_moved(rng() % key_space, index);
    }
    long replaced = 0;
    for (const auto& [order, ids] : right.model()) {
      replaced += left.model().contains(order) ? 1 : 0;
      left.model().insert_or_assign(order, ids);
    }
    right.model().clear();
    const long live_before = ledger.live();
    const bool swaps = left.map().empty();
    left.map().append(right.map());
    REQUIRE(ledger.live() == live_before - 2 * replaced);
    left.require_same();
    right.require_same();
    if (swaps && right_count > 0) {
      REQUIRE(ours.in_use() == 0);  // into an empty map the maps swap, allocators too
    } else {
      REQUIRE(theirs.in_use() == 0);  // entries moved, not pages
    }
    // Both stay usable.
    right.try_emplace_moved(1, 1);
    left.try_emplace_moved(key_space + 5, 2);
    left.erase_key(key_space + 5);
    left.require_same();
    right.require_same();
    REQUIRE(left.map().find(K(ledger, key_space + 5)) == left.map().end());
  }
  REQUIRE(ours.in_use() == 0);
  REQUIRE(theirs.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// A cut at every kind of position: both halves hold their objects, take
// changes at their new borders, and join again.
TEMPLATE_TEST_CASE("split_off cuts the objects in two and append joins them", "[map][drops]",
                   BBTREE_TEST_TRACKED_MAPS) {
  object_ledger ledger;
  page_book pages;
  std::mt19937_64 rng(31);
  for (const std::uint64_t count : {0u, 1u, 12u, 13u, 200u, 2500u}) {
    for (int round = 0; round < 16; ++round) {
      mirrored_map<TestType> lower(ledger, pages);
      for (std::uint64_t index = 0; index < count; ++index) {
        lower.try_emplace_moved(2 * index + 1, index);
      }
      const std::uint64_t cut = round == 0 ? 0 : round == 1 ? 2 * count + 5 : rng() % (2 * count + 3);
      mirrored_map<TestType> upper(ledger, pages);
      upper.map() = lower.map().split_off(lower.key(cut));
      upper.model().insert(lower.model().lower_bound(cut), lower.model().end());
      lower.model().erase(lower.model().lower_bound(cut), lower.model().end());
      lower.require_same();
      upper.require_same();
      lower.try_emplace_moved(cut == 0 ? 0 : cut - 1, 1);
      lower.try_emplace_moved(0, 2);
      upper.try_emplace_moved(cut, 3);
      upper.try_emplace_moved(2 * count + 10, 4);
      lower.pop_last();
      upper.pop_first();
      lower.require_same();
      upper.require_same();
      if (round % 2 == 0) {
        lower.map().append(upper.map());
        for (const auto& [order, ids] : upper.model()) {
          lower.model().insert_or_assign(order, ids);
        }
        upper.model().clear();
        lower.require_same();
        upper.require_same();
      }
    }
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// batch.rs: empty_single_and_repeated_keys, replaced_and_stored_values_drop_once.
TEMPLATE_TEST_CASE("batches insert and look up as the single operations do", "[map][drops][batch]",
                   bbtree_test::singly_moved_plain_keys, bbtree_test::byte_moved_paired_entries,
                   bbtree_test::move_only_singly_moved_inline_keys) {
  using K = typename TestType::key_type;
  using V = typename TestType::mapped_type;
  object_ledger ledger;
  page_book pages;
  {
    mirrored_map<TestType> maps(ledger, pages);
    std::mt19937_64 rng(33);
    // Inserts a batch of N entries with keys below `key_space`, then looks up
    // as many keys, repeated and absent ones among them.
    const auto batch = [&]<std::size_t N>(std::uint64_t key_space) {
      std::array<std::uint64_t, N> orders;
      std::array<entry_ids, N> fresh;
      std::array<std::pair<K, V>, N> entries = [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<std::pair<K, V>, N>{
            ((void)I, std::pair<K, V>(K(ledger, rng() % key_space), V(ledger, rng())))...};
      }(std::make_index_sequence<N>());
      for (std::size_t index = 0; index < N; ++index) {
        orders[index] = entries[index].first.order();
        fresh[index] = {entries[index].first.id(), entries[index].second.id()};
      }
      const std::array<bool, N> inserted = maps.map().insert_or_assign_many(std::move(entries));
      for (std::size_t index = 0; index < N; ++index) {
        REQUIRE(inserted[index] == !maps.model().contains(orders[index]));
        if (inserted[index]) {
          maps.model().emplace(orders[index], fresh[index]);
        } else {
          maps.model().at(orders[index]).value = fresh[index].value;  // the stored key stays
        }
      }
      std::vector<K> probes;
      probes.reserve(N);
      for (std::size_t index = 0; index < N; ++index) {
        probes.emplace_back(ledger, index % 3 == 0 ? rng() % (2 * key_space) : orders[index]);
      }
      std::vector<const K*> pointers;
      for (const K& probe : probes) {
        pointers.push_back(&probe);
      }
      std::vector<V*> found(N);
      maps.map().get_many(std::span<const K* const>(pointers), std::span<V*>(found));
      for (std::size_t index = 0; index < N; ++index) {
        const auto expected = maps.model().find(probes[index].order());
        REQUIRE((found[index] != nullptr) == (expected != maps.model().end()));
        if (found[index] != nullptr) {
          REQUIRE(found[index]->id() == expected->second.value);
        }
      }
    };
    for (int round = 0; round < 6; ++round) {
      const std::uint64_t key_space = round < 2 ? 40 : 6000;  // a root leaf, then a tall tree
      for (int repeat = 0; repeat < (round < 2 ? 2 : 8); ++repeat) {
        batch.template operator()<1>(key_space);
        batch.template operator()<3>(key_space);
        batch.template operator()<64>(key_space);
        batch.template operator()<65>(key_space);
        batch.template operator()<150>(key_space);
      }
      maps.require_same();
    }
    maps.map().get_many(std::span<const K* const>(), std::span<V*>());
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}
