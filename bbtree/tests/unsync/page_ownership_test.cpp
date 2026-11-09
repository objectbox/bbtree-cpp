// Where a map's and a set's pages come from and go back to, after the Rust
// crate's `unsync/map/tests/allocator.rs`: every page from the container's
// own allocator, each given back once with the layout and through the member
// it came by, and no page taken where none is needed.
#include "../support/tracked_maps.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace {

using bbtree_test::checked_allocator;
using bbtree_test::checked_map;
using bbtree_test::checked_set;
using bbtree_test::object_ledger;
using bbtree_test::page_book;

// A 64-byte key without inline keys: a dozen entries a node, so a few
// thousand make a tall tree whose internal nodes keep keys.
using wide_key = std::array<std::uint64_t, 8>;

struct wide_keys {
  static wide_key of(std::uint64_t number) {
    wide_key key;
    key.fill(number);
    return key;
  }
};

struct frame_sized_nodes : bbtree::default_node_config {
  static constexpr std::size_t leaf_page_size = 4096;
  static constexpr std::size_t internal_page_size = 4096;
};

}  // namespace

// allocator.rs: every_page_comes_from_and_goes_back_to_the_map_allocator.
TEST_CASE("every page comes from and goes back to the map's allocator", "[map][allocator]") {
  page_book pages;
  {
    const std::uint64_t count = 2000;
    std::vector<std::uint64_t> numbers(count);
    for (std::uint64_t number = 0; number < count; ++number) {
      numbers[number] = number;
    }
    std::mt19937_64 rng(21);
    std::shuffle(numbers.begin(), numbers.end(), rng);
    checked_map<wide_key, std::uint64_t> map{checked_allocator(pages)};
    for (const std::uint64_t number : numbers) {
      REQUIRE(map.insert({wide_keys::of(number), number}).second);
    }
    for (std::size_t index = 0; index < numbers.size() / 3; ++index) {
      const auto removed = map.remove(wide_keys::of(numbers[index]));
      REQUIRE(removed.has_value());
      REQUIRE(removed->second == numbers[index]);
    }
    map.pop_first();
    map.pop_last();
    REQUIRE(map.try_emplace(wide_keys::of(count), count).second);
    map.erase(std::prev(map.end()));
    checked_map<wide_key, std::uint64_t> upper = map.split_off(wide_keys::of(count / 2));
    const checked_map<wide_key, std::uint64_t> copy = upper;
    map.append(upper);
    REQUIRE(erase_if(map, [](auto entry) { return entry.first[0] % 2 == 1; }) > 0);
    map.retain([](const wide_key& key, std::uint64_t&) { return key[0] % 3 != 0; });
    map.check_invariants();
    copy.check_invariants();
    REQUIRE(pages.in_use() > 0);
    map.clear();
    map.insert({wide_keys::of(1), 1});
  }
  REQUIRE(pages.in_use() == 0);
}

// allocator.rs: a_map_of_frame_sized_nodes_asks_for_frames_alone. The root
// page is a page of the larger kind, so it too is a frame.
TEST_CASE("a map of frame-sized nodes asks for frames alone", "[map][allocator]") {
  using map_type =
      bbtree::unsync::bbtree_map<std::uint64_t, wide_key, std::less<std::uint64_t>, checked_allocator,
                                 bbtree::configured<bbtree::auto_inline_keys, frame_sized_nodes>>;
  page_book pages;
  {
    map_type map{checked_allocator(pages)};
    // Until internal nodes lie below the root: pages of every kind.
    for (std::uint64_t number = 0; number < 60'000; ++number) {
      map.insert({number * 0x9E3779B97F4A7C15ull, wide_keys::of(number)});
    }
    map.check_invariants();
    REQUIRE(pages.in_use() > 1000);
  }
  REQUIRE(pages.sizes_asked_for() == std::set<std::size_t>{4096});
  REQUIRE(pages.in_use() == 0);
}

// node/tests/leaf.rs: popping_and_reinserting_at_a_border_neither_merges_nor_splits
// (D22): a leaf that splits leaves room on both sides of the split.
TEST_CASE("popping and reinserting at a border neither merges nor splits", "[map][allocator]") {
  using map_type = checked_map<std::uint64_t, std::uint64_t>;
  const std::uint64_t capacity =
      bbtree::detail::tree_types<std::uint64_t, std::uint64_t, std::less<std::uint64_t>,
                                 bbtree::auto_inline_keys>::leaf_capacity;
  for (const bool at_front : {false, true}) {
    page_book pages;
    map_type map{checked_allocator(pages)};
    for (std::uint64_t key = 1; key <= capacity; ++key) {
      map.try_emplace(key, key);
    }
    REQUIRE(pages.in_use() == 1);  // a full root leaf
    const std::uint64_t border = at_front ? 0 : capacity + 1;
    map.try_emplace(border, border);
    REQUIRE(pages.in_use() == 3);  // a root above two leaves
    pages.refuse_after(0);         // from here on no operation may take a page
    for (int round = 0; round < 16; ++round) {
      const auto popped = at_front ? map.pop_first() : map.pop_last();
      REQUIRE(popped.has_value());
      REQUIRE(popped->first == border);
      REQUIRE(pages.in_use() == 3);
      REQUIRE(map.try_emplace(border, border).second);
      REQUIRE(pages.in_use() == 3);
      map.check_invariants();
    }
  }
}

// A map keeps its root page when its last entry is erased, and gets one
// before an insert whose value then throws: a bulk build over such a map
// replaces that page, and must give it back.
TEST_CASE("a range inserted into an emptied map gives its root page back", "[map][allocator]") {
  using map_type = checked_map<std::uint64_t, std::uint64_t>;
  using entries = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
  page_book pages;
  SECTION("ascending entries, built in place") {
    map_type map{checked_allocator(pages)};
    map.insert({1, 1});
    map.erase(1);
    REQUIRE(pages.in_use() == 1);
    const entries ascending{{1, 1}, {2, 2}, {3, 3}};
    map.insert(ascending.begin(), ascending.end());
    map.check_invariants();
    REQUIRE(map.size() == 3);
    REQUIRE(pages.in_use() == 1);
  }
  SECTION("unsorted keys of a set, sorted first") {
    checked_set<std::uint64_t> set{checked_allocator(pages)};
    set.insert(1);
    set.erase(1);
    const std::vector<std::uint64_t> unsorted{3, 1, 2};
    set.insert(unsorted.begin(), unsorted.end());
    set.check_invariants();
    REQUIRE(set.size() == 3);
    REQUIRE(pages.in_use() == 1);
  }
  SECTION("no entries at all") {
    map_type map{checked_allocator(pages)};
    map.insert({1, 1});
    map.erase(1);
    const entries none;
    map.insert(none.begin(), none.end());
    map.check_invariants();
    REQUIRE(map.empty());
    REQUIRE(pages.in_use() <= 1);
    map.insert({2, 2});
    REQUIRE(map.contains(2));
  }
  SECTION("after a first insert whose value threw") {
    using tracked_map = bbtree_test::singly_moved_inline_keys_byte_moved_values;
    using K = tracked_map::key_type;
    using V = tracked_map::mapped_type;
    object_ledger ledger;
    {
      tracked_map map{checked_allocator(pages)};
      const V prototype(ledger, 7);
      ledger.refuse_copies_after(0);
      REQUIRE_THROWS_AS(map.try_emplace(K(ledger, 1), prototype), bbtree_test::copy_refused);
      ledger.allow_copies();
      REQUIRE(map.empty());
      std::vector<std::pair<K, V>> two;
      two.emplace_back(K(ledger, 1), V(ledger, 1));
      two.emplace_back(K(ledger, 2), V(ledger, 2));
      map.insert(std::make_move_iterator(two.begin()), std::make_move_iterator(two.end()));
      map.check_invariants();
      REQUIRE(map.size() == 2);
      REQUIRE(pages.in_use() == 1);
    }
    REQUIRE(ledger.nothing_left());
  }
  REQUIRE(pages.in_use() == 0);
}

