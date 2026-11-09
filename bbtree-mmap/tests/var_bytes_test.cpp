// Keys and values of any length, kept in heap pages after their leaves, after
// the Rust crate's `bbtree-mmap/src/var_bytes/tests.rs`: what a span's check
// refuses, that a file map of spans is no file map of plain slots of their
// size, and that it holds its entries' bytes and nothing else of its writer.
#include "../../bbtree/fuzz/file_image.hpp"

#include <bbtree_mmap/bbtree_map_file.hpp>
#include <bbtree_mmap/var_bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bbtree_test {

// A plain key of a span's size and alignment, with a span's inline keys, hash
// tags and comparisons: a file map of it differs from one of var-length keys
// in what its slots hold alone.
struct alignas(8) bytes8 {
  std::array<std::uint8_t, 8> bytes;
  friend auto operator<=>(const bytes8&, const bytes8&) = default;
};

}  // namespace bbtree_test

namespace {

using bbtree_mmap::var_bytes;
using bbtree_test::bytes8;
using text_map = bbtree::unsync::bbtree_map<std::string, std::string>;
using text_map_file = bbtree_mmap::bbtree_map_file<std::string, std::string>;
// A map file of text keys and values, as a reader sees it: spans.
using span_image = bbtree_fuzz::file_image<var_bytes, var_bytes, std::less<>>;

// The numbers below `count` as text keys, each with a value of a length of
// its own, in an order `seed` shuffles: the same entries whatever the seed.
text_map text_map_of(std::size_t count, std::uint64_t seed) {
  std::vector<std::size_t> numbers(count);
  for (std::size_t number = 0; number < count; ++number) {
    numbers[number] = number;
  }
  std::shuffle(numbers.begin(), numbers.end(), std::mt19937_64(seed));
  text_map map;
  for (const std::size_t number : numbers) {
    map.insert({"key-" + std::to_string(number), std::string(number % 37, 'v')});
  }
  return map;
}

// Why a checked open of `bytes` as a file map of `K` keys and `V` values
// refuses them; nothing if it opens them.
template <class K, class V>
std::optional<bbtree::file_error::kind> refusal(std::span<const std::byte> bytes) {
  try {
    (void)bbtree::file_map<K, V, std::less<>>::open(bytes);
  } catch (const bbtree::file_error& error) {
    return error.why();
  }
  return std::nullopt;
}

// Why the check of the span at `slot` of `file_bytes` refuses it; empty if it
// passes.
std::string span_refusal(std::span<const std::byte> file_bytes, std::size_t slot) {
  try {
    bbtree::file_storage<var_bytes>::check(file_bytes, slot);
  } catch (const bbtree::file_error& error) {
    REQUIRE(error.why() == bbtree::file_error::kind::corrupt);
    return error.what();
  }
  return {};
}

}  // namespace

template <>
struct bbtree::inline_key<bytes8> {
  static bbtree::inline_word at(const bytes8& key, std::size_t offset) noexcept {
    return bbtree::detail::big_endian::window(key.bytes, offset);
  }
  static std::uint8_t hash_tag(const bytes8& key) noexcept { return key.bytes[7]; }
  static std::strong_ordering cmp_from(const bytes8& a, const bytes8& b, std::size_t equal_len) noexcept {
    return bbtree::detail::search_bytes(a.bytes).cmp_from(b.bytes, equal_len);
  }
  static std::strong_ordering cmp_prefix(const bytes8& key, std::span<const std::uint8_t> prefix) noexcept {
    return bbtree::detail::search_bytes(key.bytes).cmp_prefix(prefix);
  }
};

TEST_CASE("spans whose bytes lie outside are refused", "[mmap]") {
  std::vector<std::byte> file_bytes(64);
  const auto refusal_of = [&](std::uint32_t len, std::uint32_t offset) {
    var_bytes::encode(file_bytes.data() + 16, len, offset);
    return span_refusal(file_bytes, 16);
  };
  // Up to the file map's last byte, and no bytes right past it.
  CHECK(refusal_of(8, 40).empty());
  CHECK(refusal_of(0, 48).empty());
  const std::string outside = "a key or value's bytes outside the file map";
  CHECK(refusal_of(9, 40) == outside);
  CHECK(refusal_of(1, 0xffffffff) == outside);
  CHECK(refusal_of(0xfffffffe, 0xffffffff) == outside);
  // A query's mark: a reader would take the bytes after it for an address.
  CHECK(refusal_of(var_bytes::query_len, 8) == outside);

  // The span itself: cut off by the file map's end, past it, and so far out
  // that its own end wraps.
  CHECK(span_refusal(file_bytes, 56).empty());
  for (const std::size_t slot : {std::size_t{57}, std::size_t{64}, std::size_t{72}, ~std::size_t{0} - 3}) {
    CHECK(span_refusal(file_bytes, slot) == "a span outside the file map");
  }
}

TEST_CASE("a file map of spans is none of plain slots of a span's layout", "[mmap]") {
  const auto layout_mismatch = bbtree::file_error::kind::layout_mismatch;

  // Var-length keys: a plain key with their size, alignment and inline keys
  // would take each span's length and offset for its eight bytes.
  bbtree::unsync::bbtree_map<std::string, std::uint64_t> texts{{"key", 1}};
  const bbtree::file_buf text_keys =
      bbtree_mmap::bbtree_map_file<std::string, std::uint64_t>::bytes_of(texts);
  const auto read = bbtree::file_map<var_bytes, std::uint64_t, std::less<>>::open(text_keys.bytes());
  const bbtree_mmap::var_bytes_query key(std::string_view("key"));
  REQUIRE(read.get(key.span()) != nullptr);
  CHECK(*read.get(key.span()) == 1);
  using in_file = bbtree::detail::in_file<bbtree::auto_inline_keys>;
  using span_types = bbtree::detail::tree_types<var_bytes, std::uint64_t, std::less<>, in_file>;
  using plain_types = bbtree::detail::tree_types<bytes8, std::uint64_t, std::less<>, in_file>;
  STATIC_REQUIRE(span_types::leaf == plain_types::leaf);
  STATIC_REQUIRE(span_types::internal == plain_types::internal);
  STATIC_REQUIRE(span_types::leaf_plan.hash_tags == plain_types::leaf_plan.hash_tags);
  CHECK(refusal<bytes8, std::uint64_t>(text_keys.bytes()) == layout_mismatch);

  // Var-length values against `u64`s, of a span's size and alignment, both
  // ways: an integer taken for a span points wherever its bits say.
  bbtree::unsync::bbtree_map<std::uint64_t, std::string> names;
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> numbers;
  for (std::uint64_t key_number = 0; key_number < 3000; ++key_number) {
    names.insert({key_number, "name " + std::to_string(key_number)});
    numbers.insert({key_number, key_number % 7});
  }
  const bbtree::file_buf text_values =
      bbtree_mmap::bbtree_map_file<std::uint64_t, std::string>::bytes_of(names);
  const bbtree::file_buf plain =
      bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::bytes_of(numbers);
  CHECK(refusal<std::uint64_t, var_bytes>(text_values.bytes()) == std::nullopt);
  CHECK(refusal<std::uint64_t, std::uint64_t>(text_values.bytes()) == layout_mismatch);
  CHECK(refusal<std::uint64_t, std::uint64_t>(plain.bytes()) == std::nullopt);
  CHECK(refusal<std::uint64_t, var_bytes>(plain.bytes()) == layout_mismatch);
}

TEST_CASE("slots past a leaf's entries are zeros, not its writer's pointers", "[mmap]") {
  const std::size_t capacity = span_image::types::leaf_capacity;
  std::size_t unused = 0;
  // A count for every number of entries the last leaf can be left with: the
  // builder stocks it from the leaf before, whose slots keep what moved out.
  for (std::size_t count = 3000; count <= 3000 + capacity; ++count) {
    const span_image image(text_map_file::bytes_of(text_map_of(count, 1)).bytes());
    const std::vector<bbtree_fuzz::image_node> leaves = image.leaves();
    for (const bbtree_fuzz::image_node& leaf : leaves) {
      for (std::size_t index = image.len(leaf); index < capacity; ++index) {
        CHECK(image.is_zero(span_image::key_slot(leaf, index), sizeof(var_bytes)));
        CHECK(image.is_zero(span_image::value_slot(leaf, index), sizeof(var_bytes)));
        ++unused;
      }
    }
    // The first leaf has no head.
    CHECK(image.is_zero(span_image::head_key_slot(leaves.front()), sizeof(var_bytes)));
    CHECK(image.is_zero(span_image::head_value_slot(leaves.front()), sizeof(var_bytes)));
  }
  CHECK(unused > 0);
}

TEST_CASE("map files of var-length entries depend on the entries alone", "[mmap]") {
  for (const std::size_t count : {std::size_t{1}, std::size_t{57}, std::size_t{3001}}) {
    const bbtree::file_buf one = text_map_file::bytes_of(text_map_of(count, 1));
    const bbtree::file_buf other = text_map_file::bytes_of(text_map_of(count, 2));
    CHECK(std::equal(one.bytes().begin(), one.bytes().end(), other.bytes().begin(), other.bytes().end()));
  }
}
