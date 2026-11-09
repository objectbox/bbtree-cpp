// Refused pages and throwing copies at every point of every operation that
// can meet one, after the Rust crate's `unsync/map/tests/allocator.rs`
// (`try_insert_refused_leaves_the_map_as_it_was`) and `drops.rs`
// (`a_panicking_clone_drops_the_partial_copy`). Where the Rust crate aborts
// or hands the entry back, the port throws: `bbtree::alloc_error` from any
// page allocation, and whatever a key's or value's copy throws. PORTING.md
// ("Exceptions from user code") promises that the map is then as it was;
// here, by object id, with every page of the failed operation given back.
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
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using bbtree_test::checked_allocator;
using bbtree_test::checked_map;
using bbtree_test::copy_refused;
using bbtree_test::entry_ids;
using bbtree_test::held_entries;
using bbtree_test::mirrored_map;
using bbtree_test::object_ledger;
using bbtree_test::page_book;

// Runs an operation again and again, letting one more page or copy through
// each time before the rest are refused, until it succeeds: so that it fails
// once at each point where it allocates or copies. `after_failure` runs after
// each failure, with nothing refused. Returns how often it failed.
struct every_failure_point {
  template <class Operation, class AfterFailure>
  static long of_pages(page_book& pages, Operation operation, AfterFailure after_failure) {
    for (long allowed = 0;; ++allowed) {
      pages.refuse_after(allowed);
      try {
        operation();
        pages.refuse_nothing();
        return allowed;
      } catch (const bbtree::alloc_error&) {
        pages.refuse_nothing();
        after_failure();
      }
    }
  }

  template <class Operation, class AfterFailure>
  static long of_copies(object_ledger& ledger, long stride, Operation operation, AfterFailure after_failure) {
    for (long failures = 0;; ++failures) {
      ledger.refuse_copies_after(failures * stride);
      try {
        operation();
        ledger.allow_copies();
        return failures;
      } catch (const copy_refused&) {
        ledger.allow_copies();
        after_failure();
      }
    }
  }
};

// A map of the orders `step` apart below `count * step`, inserted in a
// random order, each with the payload `order + 7`.
template <class Map>
struct shuffled_fill {
  static void of(Map& map, object_ledger& ledger, std::uint64_t count, std::uint64_t step,
                 std::uint64_t seed) {
    std::vector<std::uint64_t> orders(count);
    for (std::uint64_t index = 0; index < count; ++index) {
      orders[index] = index * step;
    }
    std::mt19937_64 rng(seed);
    std::shuffle(orders.begin(), orders.end(), rng);
    for (const std::uint64_t order : orders) {
      map.try_emplace(typename Map::key_type(ledger, order), ledger, order + 7);
    }
  }
};

template <class Set>
std::vector<std::pair<std::uint64_t, object_ledger::object_id>> held_keys(const Set& set) {
  std::vector<std::pair<std::uint64_t, object_ledger::object_id>> held;
  for (const auto& key : set) {
    held.push_back({key.order(), key.id()});
  }
  return held;
}

}  // namespace

#define BBTREE_TEST_FAILURE_MAPS                                                                 \
  bbtree_test::singly_moved_plain_keys, bbtree_test::singly_moved_inline_keys_byte_moved_values, \
      bbtree_test::byte_moved_paired_entries

// allocator.rs: try_insert_refused_leaves_the_map_as_it_was. A split takes
// its pages before it changes anything; the caller keeps its key and value.
TEMPLATE_TEST_CASE("an insert refused a page leaves the map and its arguments as they were", "[map][failure]",
                   BBTREE_TEST_FAILURE_MAPS, bbtree_test::move_only_byte_moved_plain_keys) {
  using K = typename TestType::key_type;
  using V = typename TestType::mapped_type;
  object_ledger ledger;
  page_book pages;
  {
    mirrored_map<TestType> maps(ledger, pages);
    TestType& map = maps.map();
    std::vector<std::uint64_t> orders(3000);
    for (std::uint64_t order = 0; order < orders.size(); ++order) {
      orders[order] = order;
    }
    std::mt19937_64 rng(22);
    std::shuffle(orders.begin(), orders.end(), rng);
    long refused = 0;
    for (std::size_t index = 0; index < orders.size(); ++index) {
      K key(ledger, orders[index]);
      V value(ledger, orders[index]);
      const entry_ids fresh{key.id(), value.id()};
      const std::size_t in_use = pages.in_use();
      // Not `emplace`, which makes its entry of the arguments first, as
      // std::map's does.
      const auto insert = [&] {
        return index % 2 == 0 ? map.try_emplace(std::move(key), std::move(value)).second
                              : map.insert_or_assign(std::move(key), std::move(value)).second;
      };
      // Too few pages for some splits, none for the first insert.
      pages.refuse_after(static_cast<long>(index % 3));
      try {
        const bool inserted = insert();
        REQUIRE(inserted);
      } catch (const bbtree::alloc_error&) {
        pages.refuse_nothing();
        ++refused;
        REQUIRE_FALSE(key.moved_from());
        REQUIRE_FALSE(value.moved_from());
        REQUIRE(map.size() == maps.model().size());
        REQUIRE_FALSE(map.contains(key));
        REQUIRE(pages.in_use() == in_use);
        if (refused % 16 == 0) {
          maps.require_same();
        }
        REQUIRE(insert());
      }
      maps.model().emplace(orders[index], fresh);
    }
    pages.refuse_nothing();
    REQUIRE(refused > 20);
    maps.require_same();
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// The same with string keys behind a shared prefix, which an insert shrinks
// before it takes its pages.
TEST_CASE("an insert refused a page after shrinking the shared prefix changes no entry", "[map][failure]") {
  page_book pages;
  {
    checked_map<std::string, std::string> map{checked_allocator(pages)};
    std::map<std::string, std::string> model;
    long refused = 0;
    for (std::uint64_t number = 0; number < 4000; ++number) {
      // Now and then a key that shares less than every key before it.
      const std::string key =
          number % 500 == 499
              ? std::string("/a/sh", 5 - number / 1000) + std::to_string(number)
              : "/a/shared/prefix/that/is/long/" + std::to_string(100000 + number * 7919 % 50000);
      const std::string value = "the value of " + key;
      const std::size_t in_use = pages.in_use();
      pages.refuse_after(static_cast<long>(number % 3));
      try {
        map.try_emplace(key, value);
      } catch (const bbtree::alloc_error&) {
        pages.refuse_nothing();
        ++refused;
        REQUIRE(map.size() == model.size());
        REQUIRE_FALSE(map.contains(key));
        REQUIRE(pages.in_use() == in_use);
        if (refused % 16 == 0) {
          map.check_invariants();
        }
        map.try_emplace(key, value);
      }
      model.emplace(key, value);
    }
    pages.refuse_nothing();
    REQUIRE(refused > 50);
    map.check_invariants();
    REQUIRE(std::equal(map.begin(), map.end(), model.begin(), model.end(),
                       [](auto a, const auto& b) { return a.first == b.first && a.second == b.second; }));
  }
  REQUIRE(pages.in_use() == 0);
}

// A value is made before the tree changes: into a map without a tree, a leaf
// with room, a full leaf (after the split's pages are taken) and the
// remembered last leaf of ascending inserts.
TEMPLATE_TEST_CASE("an insert whose value throws leaves the map as it was", "[map][failure]",
                   BBTREE_TEST_FAILURE_MAPS) {
  using K = typename TestType::key_type;
  using V = typename TestType::mapped_type;
  object_ledger ledger;
  page_book pages;
  {
    mirrored_map<TestType> maps(ledger, pages);
    TestType& map = maps.map();
    const V prototype(ledger, 99);
    ledger.refuse_copies_after(0);
    REQUIRE_THROWS_AS(map.try_emplace(K(ledger, 5), prototype), copy_refused);
    ledger.allow_copies();
    // A map that has a root page and no entry answers as an empty one.
    maps.require_same();
    REQUIRE(map.find(K(ledger, 5)) == map.end());
    REQUIRE(map.lower_bound(K(ledger, 0)) == map.end());
    REQUIRE_FALSE(map.pop_first().has_value());
    REQUIRE_FALSE(map.pop_last().has_value());
    {
      const TestType copy(map);
      REQUIRE(copy.empty());
      TestType upper = map.split_off(K(ledger, 1));
      REQUIRE(upper.empty());
      map.append(upper);
    }
    std::mt19937_64 rng(41);
    long thrown = 0;
    for (std::uint64_t step = 0; step < 6000; ++step) {
      // Ascending keys, which append, then random ones.
      const std::uint64_t order = step < 2000 ? 10 * step : rng() % 20'000;
      const bool throws = step % 2 == 0;
      const bool assigns = step % 4 >= 2;
      const std::size_t in_use = pages.in_use();
      K key(ledger, order);
      const object_ledger::object_id key_id = key.id();
      ledger.refuse_copies_after(throws ? 0 : 1);
      try {
        const auto [at, inserted] = assigns ? map.insert_or_assign(std::move(key), prototype)
                                            : map.try_emplace(std::move(key), prototype);
        ledger.allow_copies();
        // Only a try_emplace of a key that is there makes no value.
        REQUIRE((!throws || (!assigns && !inserted)));
        if (inserted) {
          maps.model().emplace(order, entry_ids{key_id, at->second.id()});
        } else if (assigns) {
          maps.model().at(order).value = at->second.id();
        }
      } catch (const copy_refused&) {
        ledger.allow_copies();
        ++thrown;
        REQUIRE(throws);
        REQUIRE_FALSE(key.moved_from());
        REQUIRE(pages.in_use() == in_use);
        if (thrown % 128 == 0) {
          maps.require_same();
        }
      }
    }
    REQUIRE(thrown > 1000);
    maps.require_same();
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// An insert by `const K&` copies the key as it goes in, in a leaf with room
// or in a split, and std::string's copy can throw.
TEMPLATE_TEST_CASE("an insert whose key copy throws leaves the map as it was", "[map][failure]",
                   BBTREE_TEST_FAILURE_MAPS) {
  using K = typename TestType::key_type;
  object_ledger ledger;
  page_book pages;
  {
    mirrored_map<TestType> maps(ledger, pages);
    TestType& map = maps.map();
    std::mt19937_64 rng(43);
    long thrown = 0;
    for (std::uint64_t step = 0; step < 6000; ++step) {
      const std::uint64_t order = step < 2000 ? 10 * step : rng() % 20'000;
      const bool throws = step % 2 == 0;
      // The first insert makes the root page, which stays.
      const std::size_t in_use = std::max<std::size_t>(pages.in_use(), 1);
      const long live = ledger.live();
      const K key(ledger, order);
      ledger.refuse_copies_after(throws ? 0 : 1);
      try {
        const auto [at, inserted] = map.try_emplace(key, ledger, order);
        ledger.allow_copies();
        REQUIRE((!throws || !inserted));
        if (inserted) {
          maps.model().emplace(order, entry_ids{at->first.id(), at->second.id()});
        }
      } catch (const copy_refused&) {
        ledger.allow_copies();
        ++thrown;
        REQUIRE(throws);
        REQUIRE_FALSE(map.contains(key));
        REQUIRE(ledger.live() == live + 1);  // the probe key alone: the value made for it is gone
        REQUIRE(pages.in_use() == in_use);   // a split gave its pages back
        if (thrown % 128 == 0) {
          maps.require_same();
        }
      }
    }
    REQUIRE(thrown > 1000);
    maps.require_same();
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// drops.rs: a_panicking_clone_drops_the_partial_copy, at every page and every
// copy of a copy construction and a copy assignment.
TEMPLATE_TEST_CASE("a copy that fails part of the way takes its part apart", "[map][failure]",
                   BBTREE_TEST_FAILURE_MAPS) {
  object_ledger ledger;
  page_book pages;
  {
    TestType map{checked_allocator(pages)};
    shuffled_fill<TestType>::of(map, ledger, 700, 1, 51);
    const auto held = held_entries<TestType>::forward(map);
    const long live = ledger.live();
    const std::size_t in_use = pages.in_use();
    REQUIRE(in_use > 40);
    const auto nothing_of_the_copy_is_left = [&] {
      REQUIRE(pages.in_use() == in_use);
      REQUIRE(ledger.live() == live);
    };
    const long refusals = every_failure_point::of_pages(
        pages,
        [&] {
          const TestType copy(map);
          REQUIRE(pages.in_use() == 2 * in_use);
          REQUIRE(copy == map);
        },
        nothing_of_the_copy_is_left);
    REQUIRE(refusals >= static_cast<long>(in_use));  // a copy takes as many pages as its source
    const long throws = every_failure_point::of_copies(
        ledger, 1,
        [&] {
          const TestType copy(map);
          ledger.allow_copies();
          copy.check_invariants();
          REQUIRE(copy == map);
        },
        nothing_of_the_copy_is_left);
    REQUIRE(throws == 2 * 700);
    // A failed assignment changes neither map.
    TestType target{checked_allocator(pages)};
    shuffled_fill<TestType>::of(target, ledger, 40, 3, 52);
    const auto held_by_target = held_entries<TestType>::forward(target);
    const long live_with_target = ledger.live();
    every_failure_point::of_copies(
        ledger, 97, [&] { target = map; },
        [&] {
          REQUIRE(held_entries<TestType>::forward(target) == held_by_target);
          REQUIRE(ledger.live() == live_with_target);
        });
    REQUIRE(target == map);
    REQUIRE(held_entries<TestType>::forward(map) == held);
    map.check_invariants();
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// The range constructor and a range inserted into an empty map build in
// bulk: refused a page or a copy, the part built so far is taken apart.
TEMPLATE_TEST_CASE("a bulk build that fails part of the way leaves nothing behind", "[map][failure][build]",
                   BBTREE_TEST_FAILURE_MAPS) {
  using K = typename TestType::key_type;
  using V = typename TestType::mapped_type;
  object_ledger ledger;
  page_book pages;
  std::mt19937_64 rng(53);
  for (const bool ascending : {true, false}) {
    std::vector<std::pair<K, V>> entries;
    for (std::uint64_t index = 0; index < 600; ++index) {
      entries.emplace_back(K(ledger, ascending ? 2 * index : rng() % 450), V(ledger, index));
    }
    const long live = ledger.live();
    const auto nothing_is_left = [&] {
      REQUIRE(pages.in_use() == 0);
      REQUIRE(ledger.live() == live);
    };
    const long refusals = every_failure_point::of_pages(
        pages,
        [&] {
          const TestType built(entries.begin(), entries.end(), checked_allocator(pages));
          pages.refuse_nothing();
          built.check_invariants();
        },
        nothing_is_left);
    REQUIRE(refusals >= 5);
    {
      TestType map{checked_allocator(pages)};
      const long throws = every_failure_point::of_copies(
          ledger, 7, [&] { map.insert(entries.begin(), entries.end()); },
          [&] {
            REQUIRE(map.empty());
            REQUIRE(map.begin() == map.end());
            REQUIRE(ledger.live() == live);
          });
      REQUIRE(throws > 100);
      map.check_invariants();
      for (const auto& [key, value] : entries) {
        REQUIRE_FALSE(key.moved_from());
        REQUIRE_FALSE(value.moved_from());
        REQUIRE(map.contains(key));
      }
    }
    // Moved in and refused a page part of the way: what was moved goes with
    // the partial tree, the rest stays with the caller, and nothing leaks.
    pages.refuse_after(9);
    REQUIRE_THROWS_AS(TestType(std::make_move_iterator(entries.begin()),
                               std::make_move_iterator(entries.end()), checked_allocator(pages)),
                      bbtree::alloc_error);
    pages.refuse_nothing();
    REQUIRE(pages.in_use() == 0);
  }
  REQUIRE(ledger.nothing_left());
}

// A cut takes its pages first.
TEMPLATE_TEST_CASE("a split_off refused a page changes nothing", "[map][failure]", BBTREE_TEST_FAILURE_MAPS,
                   bbtree_test::move_only_singly_moved_inline_keys) {
  using K = typename TestType::key_type;
  object_ledger ledger;
  page_book pages;
  {
    TestType map{checked_allocator(pages)};
    shuffled_fill<TestType>::of(map, ledger, 1500, 2, 55);
    for (const std::uint64_t cut : {0u, 1u, 700u, 1501u, 2998u, 2999u, 5000u}) {
      const auto held = held_entries<TestType>::forward(map);
      const std::size_t in_use = pages.in_use();
      const long refusals = every_failure_point::of_pages(
          pages,
          [&] {
            TestType upper = map.split_off(K(ledger, cut));
            pages.refuse_nothing();
            map.check_invariants();
            upper.check_invariants();
            REQUIRE(map.size() + upper.size() == held.size());
            map.append(upper);
          },
          [&] {
            REQUIRE(pages.in_use() == in_use);
            REQUIRE(held_entries<TestType>::forward(map) == held);
          });
      REQUIRE(refusals >= 1);
      REQUIRE(held_entries<TestType>::forward(map) == held);
      map.check_invariants();
    }
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// `append` moves the entries of both maps into one bulk build, so it takes
// the build's pages before the first entry moves.
TEMPLATE_TEST_CASE("an append refused a page leaves both maps as they were", "[map][failure]",
                   BBTREE_TEST_FAILURE_MAPS, bbtree_test::move_only_singly_moved_inline_keys) {
  object_ledger ledger;
  page_book pages;
  // (entries, the step between their orders) of each map: disjoint and
  // overlapping keys, root leaves and trees of several levels.
  struct filling {
    std::uint64_t count;
    std::uint64_t step;
  };
  const std::pair<filling, filling> fillings[] = {
      {{1, 1}, {1, 1}}, {{5, 2}, {6, 3}}, {{12, 2}, {13, 2}}, {{500, 2}, {500, 3}}, {{40, 50}, {2000, 1}}};
  for (const auto& [left_filling, right_filling] : fillings) {
    TestType left{checked_allocator(pages)};
    TestType right{checked_allocator(pages)};
    shuffled_fill<TestType>::of(left, ledger, left_filling.count, left_filling.step, 57);
    shuffled_fill<TestType>::of(right, ledger, right_filling.count, right_filling.step, 58);
    const auto held_by_left = held_entries<TestType>::forward(left);
    const auto held_by_right = held_entries<TestType>::forward(right);
    const long live = ledger.live();
    const std::size_t in_use = pages.in_use();
    const long refusals = every_failure_point::of_pages(
        pages, [&] { left.append(right); },
        [&] {
          REQUIRE(pages.in_use() == in_use);
          REQUIRE(ledger.live() == live);
          REQUIRE(held_entries<TestType>::forward(left) == held_by_left);
          REQUIRE(held_entries<TestType>::forward(right) == held_by_right);
        });
    REQUIRE(refusals >= 1);
    REQUIRE(right.empty());
    left.check_invariants();
    right.check_invariants();
    // The other map's entries are all in, with their objects.
    for (const auto& [order, ids] : held_by_right) {
      const auto at = left.find(typename TestType::key_type(ledger, order));
      REQUIRE(at != left.end());
      REQUIRE(entry_ids{at->first.id(), at->second.id()} == ids);
    }
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// The same as a user of string keys sees it: no key or value turns empty.
TEST_CASE("an append of string maps refused a page keeps their entries", "[map][failure]") {
  page_book pages;
  {
    using map_type = checked_map<std::string, std::string>;
    map_type left{checked_allocator(pages)};
    map_type right{checked_allocator(pages)};
    std::map<std::string, std::string> left_model;
    std::map<std::string, std::string> right_model;
    for (int number = 0; number < 400; ++number) {
      const std::string key =
          "a key long enough to live on the heap, number " + std::to_string(1000 + number);
      const std::string value = "value " + std::to_string(number);
      (number % 2 == 0 ? left : right).try_emplace(key, value);
      (number % 2 == 0 ? left_model : right_model).emplace(key, value);
    }
    const auto same = [](const map_type& map, const std::map<std::string, std::string>& model) {
      return std::equal(map.begin(), map.end(), model.begin(), model.end(),
                        [](auto a, const auto& b) { return a.first == b.first && a.second == b.second; });
    };
    const long refusals = every_failure_point::of_pages(
        pages, [&] { left.append(right); },
        [&] {
          REQUIRE(same(left, left_model));
          REQUIRE(same(right, right_model));
        });
    REQUIRE(refusals > 6);
    left_model.merge(right_model);
    REQUIRE(same(left, left_model));
    REQUIRE(right.empty());
    left.check_invariants();
  }
  REQUIRE(pages.in_use() == 0);
}

// What `append` takes beforehand is what a build of as many entries takes,
// page for page: one less would be missed part of the way.
TEMPLATE_TEST_CASE("the pages counted for a build are the pages it takes", "[map][failure][build]",
                   bbtree_test::singly_moved_plain_keys, bbtree_test::byte_moved_paired_entries,
                   (bbtree_test::checked_map<std::uint64_t, std::uint64_t>)) {
  using K = typename TestType::key_type;
  using V = typename TestType::mapped_type;
  using types = bbtree::detail::tree_types<K, V, std::less<K>, bbtree::auto_inline_keys>;
  object_ledger ledger;
  const auto entry_of = [&](std::uint64_t order) {
    if constexpr (std::is_integral_v<K>) {
      return std::pair<K, V>(order, order);
    } else {
      return std::pair<K, V>(K(ledger, order), V(ledger, order));
    }
  };
  // Every count while the tree is small, then the ones around each new leaf,
  // far enough for a root above internal nodes.
  const std::uint64_t per_leaf = types::leaf_capacity + 1;
  const std::uint64_t most = (types::internal_capacity + 2) * per_leaf + 2;
  for (std::uint64_t count = 0; count <= most; ++count) {
    if (count > 400 && count % per_leaf > 1 && count % per_leaf != per_leaf - 1) {
      continue;
    }
    std::vector<std::pair<K, V>> entries;
    entries.reserve(count);
    for (std::uint64_t order = 0; order < count; ++order) {
      entries.push_back(entry_of(order));
    }
    page_book pages;
    const TestType built(std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()),
                         checked_allocator(pages));
    const auto counted = bbtree::detail::build_page_counts::of<types>(count);
    REQUIRE(built.size() == count);
    REQUIRE(pages.handed_out() == counted.leaf + counted.internal);
  }
}

// A batch inserts one by one: refused a page part of the way, the entries
// before it are in, the rest are not, and the map is well formed.
TEMPLATE_TEST_CASE("a batch refused a page leaves a well-formed map", "[map][failure][batch]",
                   bbtree_test::singly_moved_plain_keys, bbtree_test::byte_moved_paired_entries) {
  using K = typename TestType::key_type;
  using V = typename TestType::mapped_type;
  object_ledger ledger;
  page_book pages;
  {
    TestType map{checked_allocator(pages)};
    shuffled_fill<TestType>::of(map, ledger, 600, 4, 59);
    std::mt19937_64 rng(60);
    long refusals = 0;
    for (int round = 0; round < 60; ++round) {
      std::array<std::pair<K, V>, 48> entries = [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<std::pair<K, V>, 48>{
            ((void)I, std::pair<K, V>(K(ledger, rng() % 6000), V(ledger, rng())))...};
      }(std::make_index_sequence<48>());
      const std::size_t size_before = map.size();
      pages.refuse_after(round % 4);
      try {
        const auto inserted = map.insert_or_assign_many(std::move(entries));
        pages.refuse_nothing();
        REQUIRE(map.size() ==
                size_before + static_cast<std::size_t>(std::count(inserted.begin(), inserted.end(), true)));
      } catch (const bbtree::alloc_error&) {
        pages.refuse_nothing();
        ++refusals;
        REQUIRE(map.size() >= size_before);
      }
      map.check_invariants();
      REQUIRE(static_cast<std::size_t>(std::distance(map.begin(), map.end())) == map.size());
    }
    REQUIRE(refusals > 5);
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// The operators on two sets build their result from copies of keys.
TEMPLATE_TEST_CASE("a set operation that fails leaves its operands and no partial result", "[set][failure]",
                   bbtree_test::singly_moved_plain_key_set, bbtree_test::byte_moved_narrow_inline_key_set) {
  object_ledger ledger;
  page_book pages;
  {
    TestType small{checked_allocator(pages)};
    TestType large{checked_allocator(pages)};
    TestType other{checked_allocator(pages)};
    for (std::uint64_t index = 0; index < 30; ++index) {
      small.emplace(ledger, index * 37);
    }
    for (std::uint64_t index = 0; index < 900; ++index) {
      large.emplace(ledger, index * 2);
      other.emplace(ledger, index * 3);
    }
    const auto held_by_small = held_keys(small);
    const auto held_by_large = held_keys(large);
    const auto held_by_other = held_keys(other);
    const long live = ledger.live();
    const std::size_t in_use = pages.in_use();
    using set_operator = TestType (*)(const TestType&, const TestType&);
    const set_operator operators[] = {
        [](const TestType& a, const TestType& b) { return a | b; },
        [](const TestType& a, const TestType& b) { return a & b; },
        [](const TestType& a, const TestType& b) { return a - b; },
        [](const TestType& a, const TestType& b) { return a ^ b; },
    };
    // Walked together, and looked up in each direction.
    const std::pair<const TestType*, const TestType*> operands[] = {
        {&large, &other}, {&small, &large}, {&large, &small}};
    const auto nothing_of_the_result_is_left = [&] {
      REQUIRE(pages.in_use() == in_use);
      REQUIRE(ledger.live() == live);
    };
    for (const set_operator apply : operators) {
      for (const auto& pair : operands) {
        const TestType& left = *pair.first;
        const TestType& right = *pair.second;
        const auto build = [&] {
          const TestType result = apply(left, right);
          pages.refuse_nothing();
          ledger.allow_copies();
          result.check_invariants();
        };
        REQUIRE(every_failure_point::of_pages(pages, build, nothing_of_the_result_is_left) >= 1);
        every_failure_point::of_copies(ledger, 5, build, nothing_of_the_result_is_left);
      }
    }
    REQUIRE(held_keys(small) == held_by_small);
    REQUIRE(held_keys(large) == held_by_large);
    REQUIRE(held_keys(other) == held_by_other);
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}
