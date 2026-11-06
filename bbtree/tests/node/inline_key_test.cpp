// Windows, byte comparisons and the in-node count (`inline_key.hpp`,
// `inline_key_search.hpp`), against byte-by-byte definitions; after the Rust
// crate's `inline_key.rs` and `inline_key_search.rs` tests.
#include <bbtree/detail/node/inline_key_search.hpp>
#include <bbtree/detail/node/keys.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

using bbtree::inline_word;
using bbtree::detail::big_endian;
using bbtree::detail::search_bytes;

std::span<const std::uint8_t> as_bytes(const std::string& s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

inline_word window_of(std::span<const std::uint8_t> bytes, std::size_t offset) {
  inline_word word = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    const std::size_t at = offset + i;
    word = word << 8 | (at < bytes.size() ? bytes[at] : 0);
  }
  return word;
}

std::vector<std::uint8_t> random_bytes(std::mt19937_64& rng, std::size_t max_len) {
  std::vector<std::uint8_t> bytes(rng() % (max_len + 1));
  static constexpr std::array<std::uint8_t, 4> alphabet{0, 1, 'a', 0xff};
  for (auto& byte : bytes) {
    byte = alphabet[rng() % alphabet.size()];
  }
  return bytes;
}

}  // namespace

TEST_CASE("windows are big-endian and zero-padded", "[inline_key]") {
  CHECK(big_endian::window(as_bytes(""), 0) == 0);
  CHECK(big_endian::window(as_bytes("a"), 0) == 0x6100'0000'0000'0000);
  CHECK(big_endian::window(as_bytes("abc"), 0) == 0x6162'6300'0000'0000);
  CHECK(big_endian::window(as_bytes("abcdefgh"), 0) == 0x6162'6364'6566'6768);
  CHECK(big_endian::window(as_bytes("abcdefghij"), 2) == big_endian::window(as_bytes("cdefghij"), 0));
  CHECK(big_endian::window(as_bytes("abc"), 1) == 0x6263'0000'0000'0000);
  CHECK(big_endian::window(as_bytes("abc"), 3) == 0);
  CHECK(big_endian::window(as_bytes("abc"), SIZE_MAX) == 0);
}

TEST_CASE("windows match their bytes", "[inline_key]") {
  std::vector<std::uint8_t> bytes;
  for (std::uint8_t b = 1; b <= 24; ++b) {
    bytes.push_back(b);
  }
  for (std::size_t len = 0; len <= bytes.size(); ++len) {
    const std::span<const std::uint8_t> key(bytes.data(), len);
    for (std::size_t offset = 0; offset < len + 12; ++offset) {
      REQUIRE(big_endian::window(key, offset) == window_of(key, offset));
    }
  }
}

TEST_CASE("integer windows are their big-endian bytes", "[inline_key]") {
  std::mt19937_64 rng(42);
  for (int round = 0; round < 1000; ++round) {
    const std::uint64_t value = rng();
    std::array<std::uint8_t, 8> be{};
    for (std::size_t i = 0; i < 8; ++i) {
      be[i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
    }
    for (std::size_t offset : {0u, 1u, 3u, 7u, 8u, 9u}) {
      REQUIRE(bbtree::inline_key<std::uint64_t>::at(value, offset) == window_of(be, offset));
    }
    const auto signed_value = static_cast<std::int64_t>(value);
    const auto other = static_cast<std::int64_t>(rng());
    const inline_word a = bbtree::inline_key<std::int64_t>::at(signed_value, 0);
    const inline_word b = bbtree::inline_key<std::int64_t>::at(other, 0);
    REQUIRE((a < b) == (signed_value < other));
  }
}

TEST_CASE("differing windows order like the bytes", "[inline_key]") {
  std::mt19937_64 rng(41);
  for (int round = 0; round < 30'000; ++round) {
    const auto a = random_bytes(rng, 12);
    const auto b = random_bytes(rng, 12);
    std::size_t shared = 0;
    while (shared < std::max(a.size(), b.size()) &&
           (shared < a.size() ? a[shared] : 0) == (shared < b.size() ? b[shared] : 0)) {
      ++shared;
    }
    for (std::size_t offset = 0; offset <= shared; ++offset) {
      const inline_word wa = big_endian::window(a, offset);
      const inline_word wb = big_endian::window(b, offset);
      if (wa != wb) {
        REQUIRE((wa < wb) == std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end()));
      }
    }
  }
}

TEST_CASE("byte strings compare from any equal length as std does", "[inline_key]") {
  std::mt19937_64 rng(43);
  for (int round = 0; round < 20'000; ++round) {
    const auto a = random_bytes(rng, 40);
    auto b = a;
    if (rng() % 2 == 0) {
      b = random_bytes(rng, 40);
    } else {
      b.resize(rng() % (a.size() + 3), 'a');
    }
    std::size_t equal = 0;
    while (equal < std::min(a.size(), b.size()) && a[equal] == b[equal]) {
      ++equal;
    }
    const auto expected = std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
    const std::size_t from = equal == 0 ? 0 : rng() % (equal + 1);
    REQUIRE(search_bytes(a).cmp_from(b, from) == expected);
  }
}

TEST_CASE("byte strings compare against prefixes zero-padded", "[inline_key]") {
  std::mt19937_64 rng(44);
  for (int round = 0; round < 20'000; ++round) {
    const auto key = random_bytes(rng, 80);
    std::vector<std::uint8_t> prefix;
    if (rng() % 2 == 0) {
      prefix.assign(key.begin(), key.begin() + static_cast<std::ptrdiff_t>(rng() % (key.size() + 1)));
      prefix.resize(prefix.size() + rng() % 3, 0);
    } else {
      prefix = random_bytes(rng, 80);
    }
    std::vector<std::uint8_t> padded(prefix.size());
    for (std::size_t i = 0; i < prefix.size(); ++i) {
      padded[i] = i < key.size() ? key[i] : 0;
    }
    const auto expected =
        std::lexicographical_compare_three_way(padded.begin(), padded.end(), prefix.begin(), prefix.end());
    REQUIRE(search_bytes(key).cmp_prefix(prefix) == expected);
  }
}

namespace {

// Sorted live slots, the rest the greatest inline key; the counted equal
// range must be the sorted one.
template <bbtree::inline_width W, std::size_t Capacity>
void check_equal_ranges(std::mt19937_64& rng) {
  using slot = typename bbtree::detail::inline_slot<W>::type;
  for (int round = 0; round < 2000; ++round) {
    const std::size_t len = rng() % (Capacity + 1);
    std::vector<slot> slots(Capacity, static_cast<slot>(~slot{0}));
    for (std::size_t i = 0; i < len; ++i) {
      slots[i] = static_cast<slot>(rng() % 8);
    }
    std::sort(slots.begin(), slots.begin() + static_cast<std::ptrdiff_t>(len));
    const bbtree::detail::node_inline_keys<W, Capacity> keys(slots.data(), len);
    for (slot target = 0; target < 9; ++target) {
      const auto live_end = slots.begin() + static_cast<std::ptrdiff_t>(len);
      const auto lower =
          static_cast<std::size_t>(std::lower_bound(slots.begin(), live_end, target) - slots.begin());
      const auto upper =
          static_cast<std::size_t>(std::upper_bound(slots.begin(), live_end, target) - slots.begin());
      const auto range = keys.equal_range(bbtree::detail::inline_slot<W>::inline_key(target));
      REQUIRE(range.start == lower);
      REQUIRE(range.end == upper);
    }
  }
}

}  // namespace

TEST_CASE("the count finds the equal range at every width and capacity", "[inline_key]") {
  std::mt19937_64 rng(45);
  using bbtree::inline_width;
  check_equal_ranges<inline_width::two, 12>(rng);
  check_equal_ranges<inline_width::two, 49>(rng);
  check_equal_ranges<inline_width::two, 55>(rng);
  check_equal_ranges<inline_width::two, 64>(rng);
  check_equal_ranges<inline_width::four, 17>(rng);
  check_equal_ranges<inline_width::four, 28>(rng);
  check_equal_ranges<inline_width::four, 41>(rng);
  check_equal_ranges<inline_width::eight, 24>(rng);
  check_equal_ranges<inline_width::eight, 31>(rng);
}

TEST_CASE("hash tags find none, one, two or many candidates", "[inline_key]") {
  std::mt19937_64 rng(46);
  std::array<std::uint8_t, 24> tags{};
  for (int round = 0; round < 5000; ++round) {
    for (auto& tag : tags) {
      tag = static_cast<std::uint8_t>(rng() % 4);
    }
    const std::size_t start = rng() % 25;
    const std::size_t end = start + rng() % (25 - start);
    const auto target = static_cast<std::uint8_t>(rng() % 4);
    std::vector<std::size_t> expected;
    for (std::size_t i = start; i < end; ++i) {
      if (tags[i] == target) {
        expected.push_back(i);
      }
    }
    const auto found = bbtree::detail::hash_tags<24>(tags.data()).matching({start, end}, target);
    using count = bbtree::detail::tag_matches::count;
    switch (expected.size()) {
      case 0:
        REQUIRE(found.found == count::none);
        break;
      case 1:
        REQUIRE(found.found == count::one);
        REQUIRE(found.lower == expected[0]);
        break;
      case 2:
        REQUIRE(found.found == count::two);
        REQUIRE(found.lower == expected[0]);
        REQUIRE(found.upper == expected[1]);
        break;
      default:
        REQUIRE(found.found == count::many);
    }
  }
}
