// Keys whose order, inline keys, hash tags or `cmp_from` lie, after the Rust
// crate's `inline_key.rs` (`wrong_inline_keys_stay_memory_safe`),
// `node/tests/leaf.rs` (`a_tree_stays_memory_safe_under_any_ord`) and the
// lies of its `user_code_panics` fuzz target. The map may answer wrongly; it
// must not read outside its entries, lose or double-destroy one, or leak a
// page (PORTING.md, "Exceptions from user code"). Keys and values are
// tracked, so a wrong move shows; the library's assertions are on in the
// tests' builds, and none may fire on a lie.
#include "../support/tracked_maps.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace {

using bbtree_test::checked_allocator;
using bbtree_test::checked_map;
using bbtree_test::object_ledger;
using bbtree_test::page_book;

// A key that orders by its number until the test says otherwise: from then on
// every key is less than every other, so no search finds any.
struct fickle_key {
  std::uint64_t number;
  const bool* lies;

  friend bool operator<(const fickle_key& a, const fickle_key& b) noexcept {
    return *a.lies || a.number < b.number;
  }
};

// What a lying key lies about.
enum class lie : std::uint8_t {
  // An order that is not transitive: rock, paper, scissors.
  order,
  // Inline keys that are a hash of the key, not its leading bytes.
  windows,
  hash_tags,
  cmp_from,
  everything,
};

// A tracked key of a number, with every inline-key hook, that lies about
// `Lie`.
template <lie Lie>
class lying_key {
 public:
  lying_key(object_ledger& ledger, std::uint64_t number) : object_(ledger), number_(number) {}

  std::uint64_t number() const noexcept {
    object_.require_live();
    return number_;
  }
  static constexpr bool lies_about(lie what) noexcept { return Lie == what || Lie == lie::everything; }

  friend bool operator<(const lying_key& a, const lying_key& b) noexcept {
    if (lies_about(lie::order)) {
      const std::uint64_t step = (b.number() - a.number()) % 3;
      return step == 1 || (step == 0 && a.number() < b.number());
    }
    return a.number() < b.number();
  }
  friend bool operator==(const lying_key& a, const lying_key& b) noexcept { return a.number() == b.number(); }

 private:
  bbtree_test::tracked_object<bbtree_test::moves::one_at_a_time, bbtree_test::copies::allowed> object_;
  std::uint64_t number_;
};

}  // namespace

template <lie Lie>
struct bbtree::inline_key<lying_key<Lie>> {
  using key_type = lying_key<Lie>;
  static constexpr bbtree::inline_width width = bbtree::inline_width::four;

  static bbtree::inline_word at(const key_type& key, std::size_t offset) noexcept {
    if (key_type::lies_about(lie::windows)) {
      return (key.number() * 0x9e3779b97f4a7c15ull) >> (offset % 8);
    }
    return offset >= 8 ? 0 : key.number() << (8 * offset);
  }
  static std::uint8_t hash_tag(const key_type& key) noexcept {
    return key_type::lies_about(lie::hash_tags) ? static_cast<std::uint8_t>(key.number() * 7 >> 3)
                                                : std::uint8_t{1};
  }
  static std::strong_ordering cmp_from(const key_type& a, const key_type& b, std::size_t equal_len) noexcept {
    if (key_type::lies_about(lie::cmp_from)) {
      if ((a.number() ^ equal_len) % 3 == 0) {
        return std::strong_ordering::less;
      }
      return (b.number() + equal_len) % 3 == 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
    }
    return a.number() <=> b.number();
  }
};

// An iterator that a lookup made finds its node stack by searching its key
// (PORTING.md, P7). When that search misses, the iterator must still step
// from, and erase, the entry it is at.
TEST_CASE("an iterator stays at its entry when a search no longer finds its key", "[map][lying]") {
  using map_type = bbtree::unsync::bbtree_map<fickle_key, std::uint64_t>;
  bool lies = false;
  map_type map;
  const std::uint64_t count = 3000;
  for (std::uint64_t number = 0; number < count; ++number) {
    map.try_emplace(fickle_key{number * 7919 % count, &lies}, number * 7919 % count);
  }
  SECTION("stepping forward from every entry") {
    for (std::uint64_t number = 0; number + 1 < count; number += 3) {
      auto at = map.find(fickle_key{number, &lies});
      lies = true;
      for (std::uint64_t expected = number; expected < std::min(number + 200, count); ++expected, ++at) {
        REQUIRE(at->second == expected);
      }
      lies = false;
    }
  }
  SECTION("stepping backward from every entry") {
    for (std::uint64_t number = 0; number < count; number += 3) {
      auto at = map.find(fickle_key{number, &lies});
      lies = true;
      for (std::uint64_t expected = number; expected > 0 && expected + 200 > number; --expected, --at) {
        REQUIRE(at->second == expected);
      }
      lies = false;
    }
  }
  SECTION("erasing every other entry, separators among them") {
    for (std::uint64_t number = 0; number < count; number += 2) {
      const auto at = map.find(fickle_key{number, &lies});
      lies = true;
      const auto next = map.erase(at);
      lies = false;
      REQUIRE(next != map.end());
      REQUIRE(next->second == number + 1);
    }
    map.check_invariants();
    REQUIRE(map.size() == count / 2);
    for (std::uint64_t number = 0; number < count; ++number) {
      REQUIRE(map.contains(fickle_key{number, &lies}) == (number % 2 == 1));
    }
  }
}

TEMPLATE_TEST_CASE_SIG("a map of lying keys destroys every entry once and gives every page back",
                       "[map][lying]", ((lie Lie), Lie), lie::order, lie::windows, lie::hash_tags,
                       lie::cmp_from, lie::everything) {
  using K = lying_key<Lie>;
  using V = bbtree_test::tracked_value<bbtree_test::moves::one_at_a_time>;
  using map_type = checked_map<K, V>;
  object_ledger ledger;
  page_book pages;
  {
    map_type map{checked_allocator(pages)};
    std::mt19937_64 rng(77);
    for (int step = 0; step < 40'000; ++step) {
      const std::uint64_t number = rng() % 3000;
      switch (rng() % 16) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
          map.insert_or_assign(K(ledger, number), V(ledger, number));
          break;
        case 5:
          map.try_emplace(K(ledger, number), ledger, number);
          break;
        case 6:
        case 7:
          map.erase(K(ledger, number));
          break;
        case 8:
          rng() % 2 == 0 ? map.pop_first() : map.pop_last();
          break;
        case 9: {
          std::size_t visited = 0;
          for (auto at = map.lower_bound(K(ledger, number)); at != map.end() && visited < 200; ++at) {
            visited += at->second.payload() < 3000 ? 1u : 0u;
          }
          break;
        }
        case 10: {
          // An iterator from a lookup: stepped, or erased through.
          auto found = map.find(K(ledger, number));
          if (found != map.end()) {
            if (rng() % 2 == 0) {
              map.erase(found);
            } else {
              for (std::size_t visited = 0; found != map.end() && visited < 40; ++visited) {
                ++found;
              }
            }
          }
          break;
        }
        case 11:
          if (step % 16 == 0) {
            map_type upper = map.split_off(K(ledger, number));
            if (rng() % 2 == 0) {
              map.append(upper);
            }
          }
          break;
        case 12:
          if (step % 64 == 0) {
            map_type copy = map;
            copy.erase(K(ledger, number));
            if (rng() % 4 == 0) {
              map.append(copy);
            }
          }
          break;
        case 13:
          if (step % 32 == 0) {
            map.retain([&](const K& key, V&) { return key.number() % 5 != number % 5; });
          }
          break;
        case 14:
          if (step % 64 == 0) {
            std::vector<std::pair<K, V>> entries;
            for (std::uint64_t index = 0; index < 300; ++index) {
              entries.emplace_back(K(ledger, rng() % 3000), V(ledger, index));
            }
            map_type built(std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()),
                           checked_allocator(pages));
            std::size_t walked = 0;
            built.for_each([&](const K&, const V& value) { walked += value.payload() < 300 ? 1u : 0u; });
            REQUIRE(walked <= 300);
            if (rng() % 2 == 0) {
              map.append(built);
            }
          }
          break;
        default: {
          std::vector<K> keys;
          keys.reserve(8);
          std::array<const K*, 8> pointers;
          for (const K*& pointer : pointers) {
            pointer = &keys.emplace_back(ledger, rng() % 3000);
          }
          std::array<V*, 8> values;
          map.get_many(std::span<const K* const>(pointers), std::span<V*>(values));
          for (const V* value : values) {
            REQUIRE((value == nullptr || value->payload() < 3000));
          }
          break;
        }
      }
      REQUIRE(static_cast<long>(2 * map.size()) <= ledger.live());
    }
    std::size_t walked = 0;
    for (auto at = map.begin(); at != map.end() && walked < 100'000; ++at) {
      ++walked;
    }
  }
  REQUIRE(pages.in_use() == 0);
  REQUIRE(ledger.nothing_left());
}
