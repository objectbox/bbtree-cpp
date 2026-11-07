// `bbtree::compact_string` on its own, and as a map's key against a
// `std::map<std::string, _>` doing the same.
#include <bbtree/compact_string.hpp>
#include <bbtree/unsync/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using bbtree::compact_string;
using compact_map = bbtree::unsync::bbtree_map<compact_string, std::uint64_t, std::less<>>;

static_assert(sizeof(compact_string) == 16);
static_assert(bbtree::is_trivially_relocatable_v<compact_string>);
static_assert(std::is_nothrow_move_constructible_v<compact_string>);

// A string of `size` bytes that starts with `first`.
std::string text_of(std::size_t size, char first = 'a') {
  std::string text(size, 'x');
  for (std::size_t i = 0; i < size; ++i) {
    text[i] = static_cast<char>(first + static_cast<char>(i % 23));
  }
  return text;
}

}  // namespace

TEST_CASE("a compact string holds its bytes, short or long", "[compact_string]") {
  for (const std::size_t size : {0u, 1u, 7u, 11u, 12u, 13u, 15u, 16u, 100u, 5000u}) {
    const std::string text = text_of(size);
    const compact_string s(text);
    CHECK(s.size() == size);
    CHECK(s.empty() == (size == 0));
    CHECK(s.view() == text);
    CHECK(static_cast<std::string>(s) == text);
    // The short form holds its bytes in place, the long form elsewhere.
    const auto* object = reinterpret_cast<const char*>(&s);
    CHECK((s.data() >= object && s.data() < object + sizeof s) == (size <= compact_string::short_capacity));
  }
}

TEST_CASE("compact strings copy, move, assign and swap in both forms", "[compact_string]") {
  for (const std::size_t size : {3u, 12u, 13u, 40u}) {
    const std::string text = text_of(size);
    compact_string original(text);
    compact_string copy(original);
    CHECK(copy == text);
    CHECK(original == text);
    CHECK((size <= compact_string::short_capacity || copy.data() != original.data()));

    compact_string moved(std::move(original));
    CHECK(moved == text);
    CHECK(original.empty());  // NOLINT(bugprone-use-after-move): moved-from is empty

    compact_string assigned("other");
    assigned = copy;
    CHECK(assigned == text);
    assigned = compact_string(text_of(30, 'b'));
    CHECK(assigned == text_of(30, 'b'));
    const compact_string& same = assigned;
    assigned = same;  // self-assignment keeps it
    CHECK(assigned == text_of(30, 'b'));

    copy.swap(assigned);
    CHECK(copy == text_of(30, 'b'));
    CHECK(assigned == text);
  }
}

TEST_CASE("compact strings compare with strings, views and C strings", "[compact_string]") {
  const compact_string a("apple");
  const compact_string b(std::string("banana, a long one past fifteen"));
  CHECK(a < b);
  CHECK(a == compact_string("apple"));
  CHECK(a != b);
  CHECK(a == std::string("apple"));
  CHECK(std::string("apple") == a);
  CHECK(a == std::string_view("apple"));
  CHECK(a == "apple");
  CHECK("apple" == a);
  CHECK(a < std::string("b"));
  CHECK(std::string("b") > a);
  CHECK((a <=> b) == std::strong_ordering::less);
  CHECK(compact_string("") < compact_string(std::string_view("\0", 1)));
  CHECK(std::hash<compact_string>{}(a) == std::hash<std::string_view>{}("apple"));
  const std::unordered_set<compact_string> set{a, b};
  CHECK(set.contains(a));
}

TEST_CASE("a compact-string map takes std strings in and out", "[compact_string][map]") {
  compact_map map;
  map[std::string("one")] = 1;
  map["two"] = 2;
  map.insert({std::string("a key longer than fifteen bytes"), 3});
  map.try_emplace(std::string_view("four"), 4);
  CHECK(map.size() == 4);
  // Lookups by std::string, string_view and C string make no compact_string.
  CHECK(map.find(std::string("one"))->second == 1);
  CHECK(map.find(std::string_view("a key longer than fifteen bytes"))->second == 3);
  CHECK(map.contains("four"));
  CHECK_FALSE(map.contains(std::string("five")));
  CHECK(map.erase(std::string("two")) == 1);
  const std::string_view first = map.begin()->first;
  CHECK(first == "a key longer than fifteen bytes");
  std::string copied(map.begin()->first);
  CHECK(copied == first);
  map.check_invariants();
}

TEST_CASE("a compact-string map does what std::map<std::string> does", "[compact_string][map]") {
  std::mt19937_64 random(29);
  compact_map map;
  std::map<std::string, std::uint64_t, std::less<>> expected;
  // Sizes around the 12-byte boundary and past the 8-byte inline key, and
  // keys that share long prefixes, as paths do.
  const auto key_of = [&random] {
    const std::size_t size = random() % 40;
    std::string key = random() % 4 == 0 ? std::string("shared/prefix/") : std::string();
    for (std::size_t i = 0; i < size; ++i) {
      key.push_back(static_cast<char>('a' + random() % 4));
    }
    return key;
  };
  for (std::uint64_t step = 0; step < 60'000; ++step) {
    const std::string key = key_of();
    switch (random() % 4) {
      case 0:
      case 1:
        map.insert_or_assign(key, step);
        expected.insert_or_assign(key, step);
        break;
      case 2:
        CHECK(map.erase(key) == expected.erase(key));
        break;
      case 3: {
        const auto found = map.find(key);
        const auto wanted = expected.find(key);
        REQUIRE((found == map.end()) == (wanted == expected.end()));
        if (found != map.end()) {
          CHECK(found->second == wanted->second);
        }
        break;
      }
    }
    if (step % 10'000 == 0) {
      map.check_invariants();
    }
  }
  map.check_invariants();
  REQUIRE(map.size() == expected.size());
  auto wanted = expected.begin();
  for (const auto& [key, value] : map) {
    CHECK(key == wanted->first);
    CHECK(value == wanted->second);
    ++wanted;
  }
  // Copies are deep, and the copy outlives the original.
  compact_map copy;
  {
    const compact_map original = map;
    copy = original;
  }
  CHECK(copy.size() == expected.size());
  copy.check_invariants();
  while (const auto popped = copy.pop_first()) {
    CHECK(popped->first == expected.begin()->first);
    expected.erase(expected.begin());
  }
  CHECK(expected.empty());
}
