// The map's std::map interface: lookups, inserts, erases, iterators, bounds
// and comparisons, after the Rust crate's `unsync/map/tests`.
#include <bbtree/unsync/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../support/test_types.hpp"

namespace {

using u64_map = bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t>;
using string_map = bbtree::unsync::bbtree_map<std::string, std::string, std::less<>>;

static_assert(std::bidirectional_iterator<u64_map::iterator>);
static_assert(std::bidirectional_iterator<u64_map::const_iterator>);
static_assert(std::bidirectional_iterator<string_map::iterator>);
static_assert(std::ranges::bidirectional_range<u64_map>);
static_assert(std::bidirectional_iterator<u64_map::reverse_iterator>);
static_assert(std::bidirectional_iterator<u64_map::const_reverse_iterator>);
static_assert(std::bidirectional_iterator<string_map::reverse_iterator>);
static_assert(std::convertible_to<u64_map::reverse_iterator, u64_map::const_reverse_iterator>);
static_assert(!std::convertible_to<u64_map::const_reverse_iterator, u64_map::reverse_iterator>);
static_assert(!std::convertible_to<u64_map::iterator, u64_map::reverse_iterator>);
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

TEST_CASE("reverse iterators walk as std::map's, both ways, with their bases", "[map][iterator]") {
  // No entry, one, a root leaf, and trees of two and three levels.
  for (const std::uint64_t entries : {0u, 1u, 2u, 40u, 3000u, 30000u}) {
    u64_map map;
    std::map<std::uint64_t, std::uint64_t> model;
    for (std::uint64_t i = 0; i < entries; ++i) {
      const std::uint64_t key = i * 7919 % 100'003;
      map.insert_or_assign(key, i);
      model.insert_or_assign(key, i);
    }
    CHECK(map.rbegin().base() == map.end());
    CHECK(map.rend().base() == map.begin());
    CHECK((map.rbegin() == map.rend()) == model.empty());

    auto ours = map.rbegin();
    auto theirs = model.rbegin();
    for (; theirs != model.rend(); ++ours, ++theirs) {
      REQUIRE(ours != map.rend());
      REQUIRE(ours->first == theirs->first);
      REQUIRE((*ours).second == theirs->second);
      REQUIRE(ours.base() == map.upper_bound(theirs->first));
    }
    REQUIRE(ours == map.rend());
    while (theirs != model.rbegin()) {
      --ours;
      --theirs;
      REQUIRE(ours->first == theirs->first);
    }
    CHECK(ours == map.rbegin());

    const auto same_entry = [](const auto& a, const auto& b) {
      return a.first == b.first && a.second == b.second;
    };
    CHECK(std::equal(map.crbegin(), map.crend(), model.crbegin(), model.crend(), same_entry));
    CHECK(std::equal(std::as_const(map).rbegin(), std::as_const(map).rend(), model.rbegin(), model.rend(),
                     same_entry));
  }
}

TEST_CASE("a reverse iterator made from an iterator is at the entry before it", "[map][iterator]") {
  u64_map map;
  std::map<std::uint64_t, std::uint64_t> model;
  for (std::uint64_t i = 0; i < 5000; ++i) {
    map.insert_or_assign(i * 3, i);
    model.insert_or_assign(i * 3, i);
  }
  CHECK(u64_map::reverse_iterator(map.end()) == map.rbegin());
  CHECK(u64_map::reverse_iterator(map.begin()) == map.rend());
  CHECK(u64_map::const_reverse_iterator(map.cbegin()) == map.crend());
  std::mt19937_64 rng(11);
  for (int round = 0; round < 2000; ++round) {
    const std::uint64_t key = rng() % 15'100;
    // From a lookup's iterator, which has no node stack yet, and from a
    // bound's, which has one.
    const bool by_bound = round % 2 == 0;
    u64_map::reverse_iterator ours(by_bound ? map.lower_bound(key) : map.find(key));
    auto theirs = std::make_reverse_iterator(by_bound ? model.lower_bound(key) : model.find(key));
    for (int step = 0; step < 70; ++step) {
      REQUIRE((ours == map.rend()) == (theirs == model.rend()));
      if (theirs == model.rend()) {
        break;
      }
      REQUIRE(ours->first == theirs->first);
      // Post-increments return where they were.
      REQUIRE((ours++)->first == (theirs++)->first);
    }
    for (int step = 0; step < 5 && theirs != model.rbegin(); ++step) {
      REQUIRE((--ours)->first == (--theirs)->first);
      REQUIRE((ours--)->first == (theirs--)->first);
      ++ours;
      ++theirs;
    }
  }

  // It writes through, and converts to a const one at the same entry.
  auto last = map.rbegin();
  last->second = 77;
  CHECK(map.at(14'997) == 77);
  const u64_map::const_reverse_iterator const_last = last;
  CHECK(const_last == map.crbegin());
  CHECK(const_last->second == 77);
}

TEST_CASE("for_each visits every entry in order and writes values", "[map]") {
  string_map map;
  std::map<std::string, std::string> model;
  std::vector<std::pair<std::string, std::string>> visited;
  const auto record = [&](const std::string& key, const std::string& value) {
    visited.emplace_back(key, value);
  };
  std::as_const(map).for_each(record);
  CHECK(visited.empty());
  for (std::uint64_t i = 0; i < 5000; ++i) {
    const std::string key = "/srv/" + std::to_string(i * 7919 % 10007);
    map.insert_or_assign(key, std::to_string(i));
    model.insert_or_assign(key, std::to_string(i));
  }
  map.for_each([](const std::string&, std::string& value) { value += "!"; });
  for (auto& [key, value] : model) {
    value += "!";
  }
  std::as_const(map).for_each(record);
  CHECK(visited == std::vector<std::pair<std::string, std::string>>(model.begin(), model.end()));
}

TEST_CASE("for_each visits separators' entries without inline keys too", "[map]") {
  bbtree::unsync::bbtree_map<std::string, std::uint64_t, std::less<>, bbtree::global_allocator,
                             bbtree::plain_keys>
      map;
  std::map<std::string, std::uint64_t> model;
  for (std::uint64_t i = 0; i < 20'000; ++i) {
    const std::string key = std::to_string(i * 7919 % 100'003);
    map.insert_or_assign(key, i);
    model.insert_or_assign(key, i);
  }
  std::vector<std::pair<std::string, std::uint64_t>> visited;
  std::as_const(map).for_each(
      [&](const std::string& key, const std::uint64_t& value) { visited.emplace_back(key, value); });
  CHECK(visited == std::vector<std::pair<std::string, std::uint64_t>>(model.begin(), model.end()));
}

TEST_CASE("fold threads its accumulator through every entry in order", "[map]") {
  using vector_of_entries = std::vector<std::pair<std::string, std::uint64_t>>;
  const auto append = [](vector_of_entries entries, const std::string& key, const std::uint64_t& value) {
    entries.emplace_back(key, value);
    return entries;
  };
  bbtree::unsync::bbtree_map<std::string, std::uint64_t> inline_keyed;
  bbtree::unsync::bbtree_map<std::string, std::uint64_t, std::less<>, bbtree::global_allocator,
                             bbtree::plain_keys>
      plain;
  std::map<std::string, std::uint64_t> model;
  CHECK(inline_keyed.fold(vector_of_entries{}, append).empty());
  CHECK(inline_keyed.fold(std::uint64_t{7},
                          [](std::uint64_t sum, const auto&, const auto&) { return sum; }) == 7);
  for (std::uint64_t i = 0; i < 20'000; ++i) {
    const std::string key = std::to_string(i * 7919 % 100'003);
    inline_keyed.insert_or_assign(key, i);
    plain.insert_or_assign(key, i);
    model.insert_or_assign(key, i);
  }
  const vector_of_entries expected(model.begin(), model.end());
  CHECK(inline_keyed.fold(vector_of_entries{}, append) == expected);
  CHECK(plain.fold(vector_of_entries{}, append) == expected);
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

TEST_CASE("string literals and C strings are looked up as std::map looks them up", "[map]") {
  using map_type = bbtree::unsync::bbtree_map<std::string, std::uint64_t, std::less<>>;
  using types = bbtree::detail::tree_types<std::string, std::uint64_t, std::less<>, bbtree::auto_inline_keys>;
  // A literal and a `char*` are searched for as a `const char*`, by inline
  // keys, as a string view is.
  static_assert(bbtree::detail::converted_query<types, char[6]>::converts);
  static_assert(bbtree::detail::converted_query<types, char*>::converts);
  static_assert(!bbtree::detail::converted_query<types, const char*>::converts);
  static_assert(!bbtree::detail::converted_query<types, std::string_view>::converts);
  static_assert(bbtree::detail::search_key<types, const char*>::compares_inline);

  map_type map;
  std::map<std::string, std::uint64_t, std::less<>> model;
  for (std::uint64_t i = 0; i < 4000; ++i) {
    const std::string key = "key" + std::to_string(i * 7919 % 10'007);
    map.insert_or_assign(key, i);
    model.insert_or_assign(key, i);
  }
  map.insert_or_assign("ab", 1);
  model.insert_or_assign("ab", 1);

  const auto same_position = [&](map_type::const_iterator ours, auto theirs) {
    return theirs == model.end() ? ours == map.end() : ours != map.end() && ours->first == theirs->first;
  };
  // Every lookup, with one query expression for both maps.
  const auto check_lookups = [&](const auto& query) {
    REQUIRE(same_position(map.find(query), model.find(query)));
    REQUIRE(same_position(std::as_const(map).find(query), model.find(query)));
    REQUIRE(same_position(map.lower_bound(query), model.lower_bound(query)));
    REQUIRE(same_position(map.upper_bound(query), model.upper_bound(query)));
    REQUIRE(map.contains(query) == model.contains(query));
    REQUIRE(map.count(query) == model.count(query));
    REQUIRE((map.get(query) != nullptr) == model.contains(query));
  };
  check_lookups("key42");
  check_lookups("key");
  check_lookups("");
  check_lookups("zebra");
  // A literal with a NUL inside ends there, as a C string does.
  check_lookups("ab\0cd");
  // An array longer than its string, and pointers to it.
  char buffer[32];
  for (std::uint64_t i = 0; i < 10'100; i += 13) {
    std::snprintf(buffer, sizeof buffer, "key%llu", static_cast<unsigned long long>(i));
    check_lookups(buffer);
    char* const pointer = buffer;
    check_lookups(pointer);
    const char* const const_pointer = buffer;
    check_lookups(const_pointer);
    check_lookups(std::string_view(buffer));
  }

  CHECK(map.erase("key42") == model.erase("key42"));
  CHECK(map.erase("key42") == model.erase("key42"));
  std::snprintf(buffer, sizeof buffer, "key%d", 7);
  CHECK(map.erase(buffer) == model.erase(buffer));
  CHECK(map.size() == model.size());
  map.check_invariants();
}

TEST_CASE("integers of other types are looked up as std::map looks them up", "[map]") {
  using unsigned_types =
      bbtree::detail::tree_types<std::uint64_t, std::uint64_t, std::less<>, bbtree::auto_inline_keys>;
  using signed_types =
      bbtree::detail::tree_types<std::int64_t, std::uint64_t, std::less<>, bbtree::auto_inline_keys>;
  // What `<` converts to the key type is converted; what it converts the key
  // for is not, nor is a query that is no integer.
  static_assert(bbtree::detail::converted_query<unsigned_types, int>::converts);
  static_assert(bbtree::detail::converted_query<unsigned_types, std::int64_t>::converts);
  static_assert(bbtree::detail::converted_query<unsigned_types, bool>::converts);
  static_assert(!bbtree::detail::converted_query<unsigned_types, std::uint64_t>::converts);
  static_assert(!bbtree::detail::converted_query<unsigned_types, double>::converts);
  static_assert(bbtree::detail::converted_query<signed_types, int>::converts);
  static_assert(bbtree::detail::converted_query<signed_types, std::uint32_t>::converts);
  static_assert(!bbtree::detail::converted_query<signed_types, std::uint64_t>::converts);

  const auto check_lookups = [](const auto& map, const auto& model, const auto& query) {
    const auto same_position = [&](auto ours, auto theirs) {
      return theirs == model.end() ? ours == map.end() : ours != map.end() && ours->first == theirs->first;
    };
    REQUIRE(same_position(map.find(query), model.find(query)));
    REQUIRE(same_position(map.lower_bound(query), model.lower_bound(query)));
    REQUIRE(same_position(map.upper_bound(query), model.upper_bound(query)));
    REQUIRE(map.contains(query) == model.contains(query));
    REQUIRE(map.count(query) == model.count(query));
    REQUIRE((map.get(query) != nullptr) == model.contains(query));
  };

  SECTION("unsigned keys: a negative query is the key it wraps to") {
    bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t, std::less<>> map;
    std::map<std::uint64_t, std::uint64_t, std::less<>> model;
    const std::uint64_t keys[] = {0,           1,           5,          127,        255,       0x7fff'ffff,
                                  0x8000'0000, 0xffff'ffff, 1ull << 40, 1ull << 63, ~0ull - 5, ~0ull};
    for (const std::uint64_t key : keys) {
      map.insert_or_assign(key, key);
      model.insert_or_assign(key, key);
    }
    for (std::uint64_t i = 0; i < 3000; ++i) {
      map.insert_or_assign(1000 + i * 3, i);
      model.insert_or_assign(1000 + i * 3, i);
    }
    for (const int query : {0, 1, 5, 6, 127, 1000, 1001, 9997, 0x7fff'ffff, -1, -6, -7}) {
      check_lookups(map, model, query);
      check_lookups(map, model, static_cast<long long>(query));
      check_lookups(map, model, static_cast<short>(query));
      check_lookups(map, model, static_cast<unsigned>(query));
      check_lookups(map, model, static_cast<signed char>(query));
      check_lookups(map, model, static_cast<unsigned char>(query));
    }
    check_lookups(map, model, true);
    check_lookups(map, model, std::numeric_limits<std::int64_t>::min());
    CHECK(map.contains(-1));
    // std::map's erase takes no other key type before C++23.
    CHECK(map.erase(-1) == model.erase(~std::uint64_t{0}));
    CHECK(map.erase(-1) == model.erase(~std::uint64_t{0}));
    CHECK(map.erase(5) == model.erase(std::uint64_t{5}));
    CHECK(map.size() == model.size());
    map.check_invariants();
  }

  SECTION("signed keys") {
    bbtree::unsync::bbtree_map<std::int64_t, std::uint64_t, std::less<>> map;
    std::map<std::int64_t, std::uint64_t, std::less<>> model;
    for (std::int64_t key = -3000; key < 3000; key += 3) {
      map.insert_or_assign(key, 1);
      model.insert_or_assign(key, 1);
    }
    for (const int query : {-3001, -3000, -2999, -1, 0, 1, 3, 2997, 2998}) {
      check_lookups(map, model, query);
      check_lookups(map, model, static_cast<short>(query));
    }
    check_lookups(map, model, std::uint32_t{3});
    check_lookups(map, model, std::uint32_t{0xffff'ffff});
  }

  SECTION("128-bit keys") {
    bbtree::unsync::bbtree_map<bbtree::uint128, std::uint64_t, std::less<>> map;
    std::map<bbtree::uint128, std::uint64_t, std::less<>> model;
    for (const bbtree::uint128 key :
         {bbtree::uint128{0}, bbtree::uint128{42}, bbtree::uint128{1} << 100, ~bbtree::uint128{0}}) {
      map.insert_or_assign(key, 1);
      model.insert_or_assign(key, 1);
    }
    for (const int query : {0, 41, 42, 43, -1}) {
      check_lookups(map, model, query);
      check_lookups(map, model, static_cast<std::uint64_t>(query));
    }
  }
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

namespace {

template <class Map, class Model>
void check_equal(const Map& map, const Model& model) {
  map.check_invariants();
  REQUIRE(map.size() == model.size());
  REQUIRE(std::equal(map.begin(), map.end(), model.begin(), model.end(),
                     [](auto a, const auto& b) { return a.first == b.first && a.second == b.second; }));
}

}  // namespace

TEST_CASE("building from a range keeps the first of equal keys", "[map][build]") {
  for (std::size_t count : {0u, 1u, 12u, 55u, 56u, 1000u, 20'000u, 100'000u}) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
    std::mt19937_64 rng(count);
    for (std::size_t i = 0; i < count; ++i) {
      entries.emplace_back(rng() % (count * 2 + 1), i);
    }
    const u64_map map(entries.begin(), entries.end());
    const std::map<std::uint64_t, std::uint64_t> model(entries.begin(), entries.end());
    check_equal(map, model);
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](const auto& a, const auto& b) { return a.first == b.first; }),
                  entries.end());
    const u64_map sorted(entries.begin(), entries.end());
    check_equal(sorted, std::map<std::uint64_t, std::uint64_t>(entries.begin(), entries.end()));
  }
}

TEST_CASE("building string keys that share less as they come", "[map][build]") {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 30'000; ++i) {
    entries.emplace_back("/home/user/projects/" + std::to_string(i % 97) + "/file-" + std::to_string(i), "v");
  }
  entries.emplace_back("/home/other", "o");
  entries.emplace_back("/etc/passwd", "p");
  const bbtree::unsync::bbtree_map<std::string, std::string> map(entries.begin(), entries.end());
  check_equal(map, std::map<std::string, std::string>(entries.begin(), entries.end()));
}

TEST_CASE("a copy has the same entries and is independent", "[map][build]") {
  bbtree_test::drop_ledger ledger;
  {
    bbtree::unsync::bbtree_map<std::string, bbtree_test::tracked> map;
    for (std::uint64_t i = 0; i < 20'000; ++i) {
      map.try_emplace("key-" + std::to_string(i * 7919 % 20'011), ledger, i);
    }
    auto copy = map;
    copy.check_invariants();
    REQUIRE(copy.size() == map.size());
    REQUIRE(copy == map);
    copy.erase(copy.begin());
    REQUIRE(copy.size() + 1 == map.size());
  }
  CHECK(ledger.alive() == 0);
}

TEST_CASE("split_off cuts at every kind of position", "[map][build]") {
  for (const std::uint64_t count : {1u, 40u, 200u, 3000u, 40'000u}) {
    for (std::uint64_t cut = 0; cut <= count * 2 + 2; cut += std::max<std::uint64_t>(1, count / 13)) {
      u64_map map;
      std::map<std::uint64_t, std::uint64_t> model;
      for (std::uint64_t i = 0; i < count; ++i) {
        map.insert({i * 2, i});
        model.insert({i * 2, i});
      }
      u64_map upper = map.split_off(cut);
      std::map<std::uint64_t, std::uint64_t> model_upper(model.lower_bound(cut), model.end());
      model.erase(model.lower_bound(cut), model.end());
      check_equal(map, model);
      check_equal(upper, model_upper);
      upper.insert({cut + 1, 0});
      map.insert({0, 9});
      upper.check_invariants();
      map.check_invariants();
    }
  }
}

TEST_CASE("append merges in order, the appended values winning", "[map][build]") {
  std::mt19937_64 rng(5);
  for (int round = 0; round < 20; ++round) {
    u64_map left;
    u64_map right;
    std::map<std::uint64_t, std::uint64_t> model;
    const std::size_t left_count = rng() % 5000;
    const std::size_t right_count = rng() % 5000;
    for (std::size_t i = 0; i < left_count; ++i) {
      const std::uint64_t key = rng() % 8000;
      left.insert_or_assign(key, 1);
      model.insert_or_assign(key, 1);
    }
    for (std::size_t i = 0; i < right_count; ++i) {
      const std::uint64_t key = rng() % 8000;
      right.insert_or_assign(key, 2);
    }
    for (const auto& [key, value] : right) {
      model.insert_or_assign(key, value);
    }
    left.append(right);
    CHECK(right.empty());
    check_equal(left, model);
  }
}

TEST_CASE("erase at the iterator a lookup made, down to an empty map", "[map]") {
  // In random order, so that the entry is in a leaf with entries to spare,
  // the last of its leaf, in a leaf at its minimum, a separator, and at last
  // in the root.
  u64_map map;
  std::map<std::uint64_t, std::uint64_t> model;
  std::vector<std::uint64_t> keys;
  for (std::uint64_t i = 0; i < 8000; ++i) {
    keys.push_back(i * 3);
    map.insert({i * 3, i});
    model.insert({i * 3, i});
  }
  std::shuffle(keys.begin(), keys.end(), std::mt19937_64(21));
  for (const std::uint64_t key : keys) {
    auto next = map.erase(map.find(key));
    auto model_next = model.erase(model.find(key));
    // The returned iterator is at the next entry, and steps on from it.
    for (int steps = 0; steps < 3; ++steps) {
      REQUIRE((next == map.end()) == (model_next == model.end()));
      if (model_next == model.end()) {
        break;
      }
      REQUIRE(next->first == model_next->first);
      REQUIRE(next->second == model_next->second);
      ++next;
      ++model_next;
    }
    map.check_invariants();
  }
  CHECK(map.empty());
}

TEST_CASE("erase at an iterator moves entries that are not trivially relocatable", "[map]") {
  bbtree_test::drop_ledger ledger;
  {
    bbtree::unsync::bbtree_map<std::string, bbtree_test::tracked> map;
    std::map<std::string, std::uint64_t> model;
    for (std::uint64_t i = 0; i < 3000; ++i) {
      const std::string key = "key-" + std::to_string(i * 7919 % 3001);
      map.try_emplace(key, ledger, i);
      model.try_emplace(key, i);
    }
    std::mt19937_64 rng(22);
    while (model.size() > 100) {
      auto model_at = std::next(model.begin(), static_cast<std::ptrdiff_t>(rng() % model.size()));
      auto next = map.erase(map.find(model_at->first));
      model_at = model.erase(model_at);
      REQUIRE((next == map.end()) == (model_at == model.end()));
      if (model_at != model.end()) {
        REQUIRE(next->first == model_at->first);
      }
      REQUIRE(static_cast<std::size_t>(ledger.alive()) == model.size());
    }
    map.check_invariants();
  }
  CHECK(ledger.alive() == 0);
}

TEST_CASE("erase of a range matches std::map at every kind of range", "[map]") {
  // Counts around a leaf's entries, where a range at an end starts to be cut
  // off, and on trees of one to three levels.
  for (const std::uint64_t count : {0u, 1u, 40u, 56u, 57u, 300u, 20'000u}) {
    const std::uint64_t step = std::max<std::uint64_t>(1, count / 7);
    for (std::uint64_t from = 0; from <= count; from += step) {
      for (std::uint64_t to = from; to <= count; to += step) {
        u64_map map;
        std::map<std::uint64_t, std::uint64_t> model;
        for (std::uint64_t i = 0; i < count; ++i) {
          map.insert({i * 2, i});
          model.insert({i * 2, i});
        }
        // Empty ranges too, anywhere: nothing goes.
        const auto next = map.erase(map.lower_bound(from * 2), map.lower_bound(to * 2));
        const auto model_next = model.erase(model.lower_bound(from * 2), model.lower_bound(to * 2));
        REQUIRE((next == map.end()) == (model_next == model.end()));
        if (model_next != model.end()) {
          REQUIRE(next->first == model_next->first);
        }
        check_equal(map, model);
        // The map takes entries after it, where the range was and at its ends.
        for (const std::uint64_t key : {from * 2 + 1, std::uint64_t{0}, count * 2 + 1}) {
          map.insert({key, 1});
          model.insert({key, 1});
        }
        check_equal(map, model);
      }
    }
  }
}

TEST_CASE("erase of a range drops the values it erases", "[map]") {
  bbtree_test::drop_ledger ledger;
  {
    bbtree::unsync::bbtree_map<std::string, bbtree_test::tracked> map;
    for (std::uint64_t i = 0; i < 5000; ++i) {
      map.try_emplace("key-" + std::to_string(10'000 + i), ledger, i);
    }
    map.erase(map.find("key-13000"), map.end());
    CHECK(map.size() == 3000);
    CHECK(ledger.alive() == 3000);
    map.erase(map.begin(), map.find("key-11000"));
    CHECK(map.size() == 2000);
    CHECK(ledger.alive() == 2000);
    map.erase(map.find("key-11500"), map.find("key-12500"));
    CHECK(map.size() == 1000);
    CHECK(ledger.alive() == 1000);
    map.check_invariants();
    CHECK(map.begin()->first == "key-11000");
    map.erase(map.begin(), map.end());
    CHECK(map.empty());
    CHECK(ledger.alive() == 0);
    map.check_invariants();
  }
  CHECK(ledger.alive() == 0);
}
