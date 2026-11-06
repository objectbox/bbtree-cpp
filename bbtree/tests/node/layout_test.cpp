// The layouts the Rust crate's layout tests expect (`tree/node/layout.rs`),
// computed by the C++ layout math. `rust_string` stands in for Rust's
// 24-byte `String`: libstdc++'s `std::string` is 32 bytes.
#include <bbtree/detail/node/layout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace {

using bbtree::default_node_config;
using bbtree::inline_width;
using bbtree::detail::internal_layout;
using bbtree::detail::leaf_layout;
using bbtree::detail::node_layout;

struct rust_string {
  void* pointer;
  std::size_t capacity;
  std::size_t len;
};
static_assert(sizeof(rust_string) == 24);

struct empty_value {};

struct expected_node {
  std::size_t size;
  std::size_t align;
  std::size_t capacity;
  std::size_t keys_offset;
  std::size_t trailing_offset;
};

void check_leaf(const leaf_layout& layout, expected_node expected) {
  CHECK(layout.page.size == expected.size);
  CHECK(layout.page.align == expected.align);
  CHECK(layout.keys.capacity == expected.capacity);
  CHECK(layout.keys.keys_offset == expected.keys_offset);
  CHECK(layout.head_offset == expected.trailing_offset);
}

void check_internal(const internal_layout& layout, expected_node expected) {
  CHECK(layout.page.size == expected.size);
  CHECK(layout.page.align == expected.align);
  CHECK(layout.keys.capacity == expected.capacity);
  CHECK(layout.keys.keys_offset == expected.keys_offset);
  CHECK(layout.edges_offset == expected.trailing_offset);
}

template <class K, class V, class C = default_node_config>
using layouts = node_layout<K, V, C>;

}  // namespace

TEST_CASE("u64 to u64 fills one page", "[layout]") {
  using L = layouts<std::uint64_t, std::uint64_t>;
  check_leaf(L::leaf(std::nullopt, false, false), {1024, 1024, 63, 8, 512});
  check_internal(L::internal(std::nullopt, false), {1024, 1024, 63, 8, 512});
  CHECK(L::leaf(std::nullopt, false, false).value_stride == 8);
}

TEST_CASE("a 24-byte string to u64 fills one page", "[layout]") {
  using L = layouts<rust_string, std::uint64_t>;
  check_leaf(L::leaf(std::nullopt, false, false), {1024, 1024, 31, 8, 752});
  check_internal(L::internal(std::nullopt, false), {1024, 1024, 31, 8, 752});
}

TEST_CASE("a 24-byte string to u64 with inline keys pairs entries", "[layout]") {
  using L = layouts<rust_string, std::uint64_t>;
  const leaf_layout leaf = L::leaf(inline_width::eight, false, false);
  check_leaf(leaf, {1024, 1024, 24, 256, 248});
  CHECK(leaf.keys.key_stride == 32);
  CHECK(leaf.value_stride == 32);
  CHECK(leaf.keys.inline_keys_offset == 8);
  check_internal(L::internal(inline_width::eight, false), {512, 512, 31, 256, 256});
  check_internal(L::internal(inline_width::four, false), {512, 512, 41, 172, 176});
}

TEST_CASE("hash tags fit in the padding", "[layout]") {
  using L = layouts<rust_string, std::uint64_t>;
  const leaf_layout tagged = L::leaf(inline_width::eight, true, false);
  check_leaf(tagged, {1024, 1024, 24, 256, 248});
  CHECK(tagged.hash_tags_offset == 200);
  CHECK(tagged.head_key_offset == 224);
}

TEST_CASE("hash tags give up an entry for the head key", "[layout]") {
  using L = layouts<rust_string, rust_string>;
  const leaf_layout tagged = L::leaf(inline_width::eight, true, false);
  check_leaf(tagged, {1024, 1024, 16, 200, 176});
  CHECK(tagged.hash_tags_offset == 136);
  CHECK(tagged.head_key_offset == 152);
  CHECK(L::leaf(inline_width::eight, false, false).keys.capacity == 17);
  CHECK_FALSE(layouts<rust_string, empty_value>::leaf(inline_width::eight, true, false).hash_tags_offset);
}

TEST_CASE("u64 to u64 with two-byte inline keys", "[layout]") {
  using L = layouts<std::uint64_t, std::uint64_t>;
  check_leaf(L::leaf(inline_width::two, false, false), {1024, 1024, 55, 144, 136});
  check_internal(L::internal(inline_width::two, false), {512, 512, 49, 106, 112});
}

TEST_CASE("a set of bytes with inline keys packs one-byte entries", "[layout]") {
  check_leaf(layouts<std::uint8_t, empty_value>::leaf(inline_width::eight, false, false),
             {1024, 1024, 112, 905, 905});
}

TEST_CASE("large values grow leaves only", "[layout]") {
  using L = layouts<std::uint64_t, std::array<std::uint8_t, 1000>>;
  CHECK(L::leaf(std::nullopt, false, false).keys.capacity == 12);
  CHECK(L::internal(std::nullopt, false) ==
        layouts<std::uint64_t, std::uint64_t>::internal(std::nullopt, false));
}

TEST_CASE("the largest value that fits one page, and one byte more", "[layout]") {
  CHECK(layouts<std::uint64_t, std::array<std::uint8_t, 70>>::leaf(std::nullopt, false, false).page.size ==
        1024);
  const leaf_layout two_pages =
      layouts<std::uint64_t, std::array<std::uint8_t, 71>>::leaf(std::nullopt, false, false);
  CHECK(two_pages.page.size == 2048);
  CHECK(two_pages.keys.capacity == 24);
}

TEST_CASE("zero-sized keys and values hit the capacity cap", "[layout]") {
  CHECK(layouts<empty_value, empty_value>::leaf(std::nullopt, false, false).keys.capacity == 1023);
}

namespace {
struct large_pages : default_node_config {
  static constexpr std::size_t leaf_page_size = 4096;
  static constexpr std::size_t internal_page_size = 2048;
  static constexpr std::size_t plain_internal_page_size = 2048;
};
}  // namespace

TEST_CASE("large pages hold more keys", "[layout]") {
  using L = layouts<std::uint64_t, std::uint64_t, large_pages>;
  check_leaf(L::leaf(std::nullopt, false, false), {4096, 4096, 255, 8, 2048});
  check_internal(L::internal(std::nullopt, false), {2048, 2048, 127, 8, 1024});
  check_leaf(L::leaf(inline_width::two, false, false), {4096, 4096, 226, 480, 472});
  check_internal(L::internal(inline_width::two, false), {2048, 2048, 203, 414, 416});
}

TEST_CASE("layouts are compile-time constants", "[layout]") {
  using L = layouts<std::string, std::uint64_t>;
  static constexpr leaf_layout leaf = L::leaf(inline_width::eight, true, false);
  STATIC_CHECK(leaf.keys.capacity >= 12);
  STATIC_CHECK(leaf.page.size == 1024);
}
