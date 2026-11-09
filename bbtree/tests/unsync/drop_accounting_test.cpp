// Drop accounting for the map's single-entry operations, after the Rust
// crate's `unsync/map/tests/drops.rs`: through inserts, replacements,
// removals, retains and iterator steps, every key and value is destroyed
// once, none is copied where it should move or read after it moved, the map
// holds the very objects a model says it should, and every page goes back to
// its allocator.
#include "../support/mirrored_map.hpp"
#include "../support/tracked_maps.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <random>
#include <type_traits>
#include <vector>

namespace {

using bbtree_test::entry_ids;
using bbtree_test::mirrored_map;
using bbtree_test::object_ledger;
using bbtree_test::page_book;

// Thrown by a retain's predicate that stops part of the way.
struct predicate_stopped {};

template <class Map>
constexpr bool copies_entries = std::is_copy_constructible_v<typename Map::key_type>;

}  // namespace

// drops.rs: drops_match_std, entries_drop_like_std, replacing_keeps_the_old_key_and_drops_each_once,
// replacing_separators_matches_std.
TEMPLATE_TEST_CASE("random single-entry operations hold the model's objects", "[map][drops]",
                   BBTREE_TEST_TRACKED_MAPS) {
  object_ledger ledger;
  page_book pages;
  {
    mirrored_map<TestType> maps(ledger, pages);
    std::mt19937_64 rng(21);
    for (int step = 0; step < 30'000; ++step) {
      const std::uint64_t order = rng() % 4000;
      const std::uint64_t payload = rng();
      switch (rng() % 13) {
        case 0:
        case 1:
        case 2:
          maps.try_emplace_moved(order, payload);
          break;
        case 3:
        case 4:
          maps.insert_or_assign_moved(order, payload);
          break;
        case 5:
          maps.emplace_pair(order, payload);
          break;
        case 6:
          if constexpr (copies_entries<TestType>) {
            maps.try_emplace_copied(order, payload);
          }
          break;
        case 7:
          if constexpr (copies_entries<TestType>) {
            maps.insert_value_type(order, payload);
          }
          break;
        case 8:
          maps.assign_through_iterator(order, payload);
          break;
        case 9:
          maps.erase_key(order);
          break;
        case 10:
          maps.erase_at(order);
          break;
        case 11:
          maps.remove_entry(order);
          break;
        default:
          maps.look_up(order);
          break;
      }
      if (step % 2048 == 0) {
        maps.require_same();
      }
    }
    maps.require_same();
    REQUIRE(pages.in_use() > 100);  // a tall tree
    // Every key a second time, separators among them: the stored key stays.
    const auto held = maps.model();
    for (const auto& [order, ids] : held) {
      maps.insert_or_assign_moved(order, order);
      maps.look_up(order);
    }
    maps.require_same();
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// drops.rs: removals_drop_like_std. Every way out of the map hands back, or
// destroys, the stored objects: by key, by iterator, from both ends, down to
// nothing, separators among them.
TEMPLATE_TEST_CASE("removals hand back the stored objects", "[map][drops]", BBTREE_TEST_TRACKED_MAPS) {
  object_ledger ledger;
  page_book pages;
  {
    mirrored_map<TestType> maps(ledger, pages);
    std::mt19937_64 rng(24);
    const std::uint64_t count = 3000;
    maps.fill_shuffled(count, rng);
    maps.require_same();
    for (int step = 0; step < 12'000; ++step) {
      const std::uint64_t order = rng() % count;
      switch (rng() % 9) {
        case 0:
        case 1:
        case 2:
          maps.insert_or_assign_moved(order, rng());
          break;
        case 3:
          maps.erase_key(order);
          break;
        case 4:
          maps.remove_entry(order);
          break;
        case 5:
          maps.pop_first();
          break;
        case 6:
          maps.pop_last();
          break;
        case 7:
          maps.erase_at(order);
          break;
        default:
          maps.look_up(order);
          break;
      }
      if (step % 2048 == 0) {
        maps.require_same();
      }
    }
    maps.require_same();
    std::vector<std::uint64_t> orders;
    for (const auto& [order, ids] : maps.model()) {
      orders.push_back(order);
    }
    std::shuffle(orders.begin(), orders.end(), rng);
    for (std::size_t index = 0; index < orders.size(); ++index) {
      switch (rng() % 5) {
        case 0:
          maps.pop_first();
          break;
        case 1:
          maps.pop_last();
          break;
        case 2:
          maps.erase_at(orders[index]);
          break;
        case 3:
          maps.erase_key(orders[index]);
          break;
        default:
          maps.remove_entry(orders[index]);
          break;
      }
      if (index % 256 == 0) {
        maps.require_same();
      }
    }
    while (!maps.model().empty()) {
      maps.pop_first();
    }
    maps.require_same();
    REQUIRE(maps.map().begin() == maps.map().end());
    REQUIRE(ledger.live() == 0);
    // An emptied map takes entries again.
    maps.try_emplace_moved(7, 7);
    maps.require_same();
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// drops.rs: retain_and_extract_if_drop_like_std; extract_if.rs:
// predicate_panic_keeps_current_and_unvisited_entries. A removed entry is
// destroyed before the predicate runs again, and a predicate that throws
// leaves the entry it was asked about and every one after it.
TEMPLATE_TEST_CASE("retain destroys what it removes and stops where its predicate throws", "[map][drops]",
                   BBTREE_TEST_TRACKED_MAPS) {
  using K = typename TestType::key_type;
  using V = typename TestType::mapped_type;
  object_ledger ledger;
  page_book pages;
  {
    mirrored_map<TestType> maps(ledger, pages);
    std::mt19937_64 rng(28);
    const std::uint64_t count = 2500;
    for (int round = 0; round < 6; ++round) {
      for (std::uint64_t order = 0; order < count; ++order) {
        maps.try_emplace_moved(order, order);
      }
      std::vector<bool> removes(count);
      for (std::uint64_t order = 0; order < count; ++order) {
        removes[order] = rng() % (round % 2 == 0 ? 4 : 2) == 0;
      }
      const std::optional<std::size_t> stops_at =
          round % 3 == 2 ? std::optional<std::size_t>(rng() % (count / 2) + 1) : std::nullopt;
      std::size_t calls = 0;
      std::size_t removed = 0;
      std::optional<entry_ids> rejected;
      const auto keep = [&](const K& key, V& value) {
        if (rejected) {
          REQUIRE_FALSE(ledger.is_live(rejected->key));
          REQUIRE_FALSE(ledger.is_live(rejected->value));
          rejected.reset();
        }
        REQUIRE(entry_ids{key.id(), value.id()} == maps.model().at(key.order()));
        if (++calls == stops_at) {
          throw predicate_stopped();
        }
        if (!removes[key.order()]) {
          return true;
        }
        rejected = entry_ids{key.id(), value.id()};
        maps.model().erase(key.order());
        ++removed;
        return false;
      };
      try {
        // Outside the assertion, which would take the predicate's exception
        // for a failure.
        const std::size_t counted =
            round % 2 == 0
                ? maps.map().retain(keep)
                : erase_if(maps.map(), [&](auto entry) { return !keep(entry.first, entry.second); });
        REQUIRE(counted == removed);
        REQUIRE_FALSE(stops_at.has_value());
      } catch (const predicate_stopped&) {
        REQUIRE(calls == stops_at);
      }
      maps.require_same();
    }
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// Ranges between two bounds, empty ones among them, and the whole map.
TEMPLATE_TEST_CASE("erasing a range destroys its entries and returns the next", "[map][drops]",
                   BBTREE_TEST_TRACKED_MAPS) {
  object_ledger ledger;
  page_book pages;
  {
    mirrored_map<TestType> maps(ledger, pages);
    std::mt19937_64 rng(29);
    const std::uint64_t count = 2500;
    for (std::uint64_t order = 0; order < count; ++order) {
      maps.try_emplace_moved(2 * order, order);
    }
    for (int round = 0; round < 60; ++round) {
      const std::uint64_t low = rng() % (2 * count + 10);
      const std::uint64_t high = low + rng() % (round % 4 == 0 ? count : 40);
      const auto next =
          maps.map().erase(maps.map().lower_bound(maps.key(low)), maps.map().lower_bound(maps.key(high)));
      maps.model().erase(maps.model().lower_bound(low), maps.model().lower_bound(high));
      const auto expected_next = maps.model().lower_bound(high);
      REQUIRE((next == maps.map().end()) == (expected_next == maps.model().end()));
      if (expected_next != maps.model().end()) {
        maps.require_at(next, expected_next->first);
      }
      if (round % 8 == 0) {
        maps.require_same();
      }
    }
    maps.require_same();
    REQUIRE(maps.map().erase(maps.map().begin(), maps.map().end()) == maps.map().end());
    maps.model().clear();
    maps.require_same();
    REQUIRE(ledger.live() == 0);
    maps.try_emplace_moved(1, 1);
    maps.map().clear();
    maps.model().clear();
    maps.require_same();
    REQUIRE(ledger.live() == 0);
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}

// An iterator that a lookup or an insert made gets its node stack on its
// first crossing of a leaf boundary (PORTING.md, P7): steps both ways from
// every entry of a tall tree.
TEMPLATE_TEST_CASE("iterators from lookups and inserts step through the tree both ways", "[map][drops]",
                   bbtree_test::singly_moved_plain_keys, bbtree_test::byte_moved_paired_entries,
                   bbtree_test::move_only_singly_moved_inline_keys) {
  object_ledger ledger;
  page_book pages;
  {
    mirrored_map<TestType> maps(ledger, pages);
    std::mt19937_64 rng(35);
    const std::uint64_t count = 1200;
    const std::uint64_t reach = 40;
    maps.fill_shuffled(count, rng);
    const TestType& map = maps.map();
    for (std::uint64_t order = 0; order < count; ++order) {
      auto forward = map.find(maps.key(order));
      for (std::uint64_t at = order; at < std::min(order + reach, count); ++at, ++forward) {
        maps.require_at(forward, at);
      }
      if (order + reach >= count) {
        REQUIRE(forward == map.end());
        maps.require_at(--forward, count - 1);
      }
      auto backward = map.find(maps.key(order));
      for (std::uint64_t at = order;; --at, --backward) {
        maps.require_at(backward, at);
        if (at == 0 || at + reach <= order) {
          break;
        }
      }
      // From an insert of a key that is there, and against the bounds.
      auto existing = maps.map().try_emplace(maps.key(order), ledger, std::uint64_t{0}).first;
      maps.require_at(existing, order);
      if (order + 1 < count) {
        maps.require_at(++existing, order + 1);
        REQUIRE(maps.map().upper_bound(maps.key(order)) == existing);
        const typename TestType::const_iterator converted = existing;
        REQUIRE(converted == map.lower_bound(maps.key(order + 1)));
      }
    }
    REQUIRE(static_cast<std::size_t>(std::distance(map.begin(), map.end())) == map.size());
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}
