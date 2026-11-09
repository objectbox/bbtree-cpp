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

using bbtree::inline_width;
using bbtree::detail::slot_range;

// `Capacity` slots at width `W`, as a node holds them, and no byte more: a
// vector load that leaves the column is one AddressSanitizer reports.
template <inline_width W, std::size_t Capacity>
class slot_column {
  using slot_traits = bbtree::detail::inline_slot<W>;
  using slot = typename slot_traits::type;
  static constexpr slot greatest = static_cast<slot>(~slot{0});
  static constexpr slot sign_bit = static_cast<slot>(slot{1} << (slot_traits::bits - 1));

  // Inline keys to search among and for: both ends of their range, both
  // sides of the sign bit, which the vector compares flip, and some between.
  static constexpr std::array<slot, 12> notable{0,
                                                1,
                                                2,
                                                7,
                                                static_cast<slot>(sign_bit - 2),
                                                static_cast<slot>(sign_bit - 1),
                                                sign_bit,
                                                static_cast<slot>(sign_bit + 1),
                                                static_cast<slot>(sign_bit + 9),
                                                static_cast<slot>(greatest - 2),
                                                static_cast<slot>(greatest - 1),
                                                greatest};

 public:
  slot_column() : slots_(Capacity, greatest) {}

  // For every length up to the capacity: sorted slots with ties, the slots
  // past the length the greatest (invariant 4), and targets below, among
  // and above them. Every way of counting must find std's bounds.
  void check_sorted_equal_ranges(std::mt19937_64& rng) {
    for (std::size_t len = 0; len <= Capacity; ++len) {
      for (const std::size_t choices : {std::size_t{1}, std::size_t{3}, notable.size()}) {
        fill_sorted(rng, len, choices);
        for (const slot target : notable) {
          const auto lower = std::lower_bound(slots_.begin(), live_end(len), target) - slots_.begin();
          const auto upper = std::upper_bound(slots_.begin(), live_end(len), target) - slots_.begin();
          for (const slot_range found : equal_ranges(len, target)) {
            if (found.start != static_cast<std::size_t>(lower) ||
                found.end != static_cast<std::size_t>(upper)) {
              FAIL("width " << sizeof(slot) << ", capacity " << Capacity << ", len " << len << ", target "
                            << +target << ": " << found.start << ".." << found.end << ", expected " << lower
                            << ".." << upper);
            }
          }
        }
      }
    }
  }

  // Whatever the slots hold, as a reader racing a writer may see them, every
  // way of counting finds a range within the live slots.
  void check_any_equal_ranges_stay_in_bounds(std::mt19937_64& rng) {
    for (int round = 0; round < 200; ++round) {
      for (slot& value : slots_) {
        value = notable[rng() % notable.size()];
      }
      const std::size_t len = rng() % (Capacity + 1);
      for (const slot target : notable) {
        for (const slot_range found : equal_ranges(len, target)) {
          if (found.start > found.end || found.end > len) {
            FAIL("width " << sizeof(slot) << ", capacity " << Capacity << ", len " << len << ", target "
                          << +target << ": " << found.start << ".." << found.end);
          }
        }
      }
    }
  }

 private:
  auto live_end(std::size_t len) { return slots_.begin() + static_cast<std::ptrdiff_t>(len); }

  // `len` sorted live slots drawn from `choices` neighbouring notable inline
  // keys, the rest the greatest.
  void fill_sorted(std::mt19937_64& rng, std::size_t len, std::size_t choices) {
    std::fill(slots_.begin(), slots_.end(), greatest);
    const std::size_t first_choice = rng() % (notable.size() - choices + 1);
    for (std::size_t index = 0; index < len; ++index) {
      slots_[index] = notable[first_choice + rng() % choices];
    }
    std::sort(slots_.begin(), live_end(len));
  }

  // The equal range of `target` among the first `len` slots by every way
  // this build has of finding it: its own choice, then each one named.
  std::vector<slot_range> equal_ranges(std::size_t len, slot target) const {
    const bbtree::detail::node_inline_keys<W, Capacity> keys(slots_.data(), len);
    std::vector<slot_range> ranges{keys.equal_range(slot_traits::inline_key(target)),
                                   keys.equal_range_by_count(target), keys.equal_range_by_two_counts(target),
                                   keys.equal_range_by_binary_search(target)};
#if defined(__SSE2__)
    if constexpr (bbtree::detail::vector_count<bbtree::detail::sse2_lanes<slot>, Capacity>::fits) {
      ranges.push_back(keys.template equal_range_in_vectors<bbtree::detail::sse2_lanes<slot>>(target));
    }
#endif
#if defined(__AVX2__)
    if constexpr (bbtree::detail::vector_count<bbtree::detail::avx2_lanes<slot>, Capacity>::fits) {
      ranges.push_back(keys.template equal_range_in_vectors<bbtree::detail::avx2_lanes<slot>>(target));
    }
#endif
    return ranges;
  }

  std::vector<slot> slots_;
};

// Columns of width `W` at each of `Capacity`.
template <inline_width W, std::size_t... Capacity>
struct slot_columns {
  template <class Check>
  static void for_each(Check check) {
    (check(slot_column<W, Capacity>()), ...);
  }
};

// The capacities of the maps' nodes at each width (layout_test.cpp), those
// around a register's and a mask word's worth of slots, and large ones.
template <class Check>
void for_each_slot_column(Check check) {
  slot_columns<inline_width::two, 12, 13, 15, 16, 17, 23, 28, 31, 32, 33, 38, 48, 49, 55, 63, 64, 65, 100,
               127, 128, 129, 511, 1023>::for_each(check);
  slot_columns<inline_width::four, 12, 13, 15, 16, 17, 28, 31, 32, 33, 40, 41, 63, 64, 65, 100, 129,
               256>::for_each(check);
  slot_columns<inline_width::eight, 12, 13, 15, 16, 17, 19, 24, 28, 31, 32, 33, 41, 63, 64, 65,
               128>::for_each(check);
}

}  // namespace

TEST_CASE("every count finds the equal range at every width, capacity and length", "[inline_key]") {
  std::mt19937_64 rng(45);
  for_each_slot_column([&rng](auto column) { column.check_sorted_equal_ranges(rng); });
}

TEST_CASE("every count stays within the live slots whatever they hold", "[inline_key]") {
  std::mt19937_64 rng(47);
  for_each_slot_column([&rng](auto column) { column.check_any_equal_ranges_stay_in_bounds(rng); });
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

TEST_CASE("a byte string's window is its bytes, big-endian and zero-padded", "[inline_key]") {
  std::uint8_t bytes[24];
  for (std::size_t b = 0; b < sizeof bytes; ++b) {
    bytes[b] = static_cast<std::uint8_t>(0x80 + 13 * b);
  }
  for (std::size_t len = 0; len < sizeof bytes; ++len) {
    for (std::size_t offset = 0; offset < len + 5; ++offset) {
      std::uint64_t expected = 0;
      for (std::size_t b = 0; b < 8; ++b) {
        const std::size_t at = offset + b;
        expected = (expected << 8) | (at < len ? bytes[at] : 0);
      }
      INFO("len " << len << ", offset " << offset);
      REQUIRE(bbtree::detail::big_endian::window({bytes, len}, offset) == expected);
    }
  }
}
