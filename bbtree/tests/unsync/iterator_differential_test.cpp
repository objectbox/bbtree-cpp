// Iterators against std::map's under random moves: steps both ways across
// leaves and separators, iterators from lookups and bounds, equality between
// iterators reached different ways, and erase at an iterator, on trees of
// three levels, with inline keys and without.
#include <bbtree/unsync/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

template <class K>
K key_of(std::uint64_t n) {
  if constexpr (std::is_same_v<K, std::string>) {
    // A shared prefix, so that nodes strip it, and lengths that vary.
    return "prefix/" + std::to_string(n * 7919 % 1000003) + std::string(n % 5, 'x');
  } else {
    return n * 0x9E3779B97F4A7C15ull;
  }
}

template <class Map, class K>
void check_random_moves(std::uint64_t seed) {
  Map map;
  std::map<K, std::uint64_t> expected;
  std::mt19937_64 rng(seed);
  constexpr std::uint64_t key_space = 40000;
  for (std::uint64_t i = 0; i < 20000; ++i) {
    const std::uint64_t n = rng() % key_space;
    map.insert_or_assign(key_of<K>(n), n);
    expected.insert_or_assign(key_of<K>(n), n);
  }
  REQUIRE(map.size() == expected.size());

  auto ours = map.begin();
  auto theirs = expected.begin();
  const auto same = [&] {
    if (theirs == expected.end()) {
      return ours == map.end();
    }
    return ours != map.end() && ours->first == theirs->first && ours->second == theirs->second;
  };
  for (int move = 0; move < 200000; ++move) {
    switch (rng() % 9) {
      case 0:
      case 1:
      case 2:
        if (theirs != expected.end()) {
          ++ours;
          ++theirs;
        }
        break;
      case 3:
      case 4:
        if (theirs != expected.begin()) {
          --ours;
          --theirs;
        }
        break;
      case 5: {
        const K key = key_of<K>(rng() % key_space);
        ours = map.find(key);
        theirs = expected.find(key);
        break;
      }
      case 6: {
        const K key = key_of<K>(rng() % key_space);
        if (rng() % 2 == 0) {
          ours = map.lower_bound(key);
          theirs = expected.lower_bound(key);
        } else {
          ours = map.upper_bound(key);
          theirs = expected.upper_bound(key);
        }
        break;
      }
      case 7:
        // An iterator reached by steps equals the one a lookup finds.
        if (theirs != expected.end()) {
          REQUIRE(map.find(theirs->first) == ours);
        }
        break;
      case 8:
        if (theirs != expected.end() && expected.size() > 1000) {
          ours = map.erase(ours);
          theirs = expected.erase(theirs);
        } else {
          const std::uint64_t n = rng() % key_space;
          map.insert_or_assign(key_of<K>(n), n);
          expected.insert_or_assign(key_of<K>(n), n);
          ours = map.find(key_of<K>(n));
          theirs = expected.find(key_of<K>(n));
        }
        break;
    }
    REQUIRE(same());
  }
  REQUIRE(map.size() == expected.size());
  CHECK(std::equal(map.begin(), map.end(), expected.begin(), expected.end(),
                   [](const auto& a, const auto& b) { return a.first == b.first && a.second == b.second; }));
}

}  // namespace

TEST_CASE("iterators move as std::map's, u64 keys", "[map][iterator]") {
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    check_random_moves<bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t>, std::uint64_t>(seed);
  }
}

TEST_CASE("iterators move as std::map's, string keys", "[map][iterator]") {
  check_random_moves<bbtree::unsync::bbtree_map<std::string, std::uint64_t>, std::string>(4);
}

TEST_CASE("iterators move as std::map's, keys without inline keys", "[map][iterator]") {
  check_random_moves<bbtree::unsync::bbtree_map<std::string, std::uint64_t, std::less<std::string>,
                                                bbtree::global_allocator, bbtree::plain_keys>,
                     std::string>(5);
}
