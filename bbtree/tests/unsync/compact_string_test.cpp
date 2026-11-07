// `bbtree::basic_compact_string`, in both size types and both sizes, on its
// own and as a map's key against a `std::map<std::string, _>` doing the
// same.
#include <bbtree/compact_string.hpp>
#include <bbtree/unsync/map.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using compact_u16_16 = bbtree::basic_compact_string<std::uint16_t, 14>;
using compact_u16_24 = bbtree::basic_compact_string<std::uint16_t, 22>;

static_assert(sizeof(bbtree::compact_string) == 16 && bbtree::compact_string::short_capacity == 12);
static_assert(sizeof(bbtree::compact_string_24) == 24 && bbtree::compact_string_24::short_capacity == 20);
static_assert(sizeof(compact_u16_16) == 16 && sizeof(compact_u16_24) == 24);
static_assert(bbtree::is_trivially_relocatable_v<bbtree::compact_string>);
static_assert(std::is_nothrow_move_constructible_v<bbtree::compact_string>);

template <class S>
using compact_map = bbtree::unsync::bbtree_map<S, std::uint64_t, std::less<>>;

// A string of `size` bytes that starts with `first`.
std::string text_of(std::size_t size, char first = 'a') {
  std::string text(size, 'x');
  for (std::size_t i = 0; i < size; ++i) {
    text[i] = static_cast<char>(first + static_cast<char>(i % 23));
  }
  return text;
}

}  // namespace

#define COMPACT_STRINGS bbtree::compact_string, bbtree::compact_string_24, compact_u16_16, compact_u16_24

TEMPLATE_TEST_CASE("a compact string holds its bytes, short or long", "[TestType]", COMPACT_STRINGS) {
  constexpr std::size_t cap = TestType::short_capacity;
  for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{7}, cap - 1, cap, cap + 1,
                                 cap + 3, std::size_t{100}, std::size_t{5000}}) {
    const std::string text = text_of(size);
    const TestType s(text);
    CHECK(s.size() == size);
    CHECK(s.empty() == (size == 0));
    CHECK(s.view() == text);
    CHECK(static_cast<std::string>(s) == text);
    // The short form holds its bytes in place, the long form elsewhere.
    const auto* object = reinterpret_cast<const char*>(&s);
    CHECK((s.data() >= object && s.data() < object + sizeof s) == (size <= TestType::short_capacity));
  }
}

TEMPLATE_TEST_CASE("compact strings copy, move, assign and swap in both forms", "[TestType]",
                   COMPACT_STRINGS) {
  constexpr std::size_t cap = TestType::short_capacity;
  for (const std::size_t size : {std::size_t{3}, cap, cap + 1, std::size_t{40}}) {
    const std::string text = text_of(size);
    TestType original(text);
    TestType copy(original);
    CHECK(copy == text);
    CHECK(original == text);
    CHECK((size <= TestType::short_capacity || copy.data() != original.data()));

    TestType moved(std::move(original));
    CHECK(moved == text);
    CHECK(original.empty());  // NOLINT(bugprone-use-after-move): moved-from is empty

    TestType assigned("other");
    assigned = copy;
    CHECK(assigned == text);
    assigned = TestType(text_of(30, 'b'));
    CHECK(assigned == text_of(30, 'b'));
    const TestType& same = assigned;
    assigned = same;  // self-assignment keeps it
    CHECK(assigned == text_of(30, 'b'));

    copy.swap(assigned);
    CHECK(copy == text_of(30, 'b'));
    CHECK(assigned == text);
  }
}

TEMPLATE_TEST_CASE("compact strings compare with strings, views and C strings", "[TestType]",
                   COMPACT_STRINGS) {
  const TestType a("apple");
  const TestType b(std::string("banana, a long one past fifteen"));
  CHECK(a < b);
  CHECK(a == TestType("apple"));
  CHECK(a != b);
  CHECK(a == std::string("apple"));
  CHECK(std::string("apple") == a);
  CHECK(a == std::string_view("apple"));
  CHECK(a == "apple");
  CHECK("apple" == a);
  CHECK(a < std::string("b"));
  CHECK(std::string("b") > a);
  CHECK((a <=> b) == std::strong_ordering::less);
  CHECK(TestType("") < TestType(std::string_view("\0", 1)));
  CHECK(std::hash<TestType>{}(a) == std::hash<std::string_view>{}("apple"));
  const std::unordered_set<TestType> set{a, b};
  CHECK(set.contains(a));
}

TEMPLATE_TEST_CASE("a compact-string map takes std strings in and out", "[TestType][map]", COMPACT_STRINGS) {
  compact_map<TestType> map;
  map[std::string("one")] = 1;
  map["two"] = 2;
  map.insert({std::string("a key longer than fifteen bytes"), 3});
  map.try_emplace(std::string_view("four"), 4);
  CHECK(map.size() == 4);
  // Lookups by std::string, string_view and C string make no compact string.
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

TEMPLATE_TEST_CASE("a compact-string map does what std::map<std::string> does", "[TestType][map]",
                   COMPACT_STRINGS) {
  std::mt19937_64 random(29);
  compact_map<TestType> map;
  std::map<std::string, std::uint64_t, std::less<>> expected;
  // Sizes around the 12-, 14-, 20- and 22-byte boundaries and past the 8-byte inline key, and
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
  compact_map<TestType> copy;
  {
    const compact_map<TestType> original = map;
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

TEMPLATE_TEST_CASE("a compact string refuses what its size type can't count", "[compact_string]",
                   COMPACT_STRINGS) {
  const std::string longest(TestType::max_size < 100'000 ? TestType::max_size : 100'000, 'k');
  CHECK(TestType(longest).view() == longest);
  if constexpr (TestType::max_size < 100'000) {
    CHECK_THROWS_AS(TestType(longest + "k"), std::length_error);
  }
}
