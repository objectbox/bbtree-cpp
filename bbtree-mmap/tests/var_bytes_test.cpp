// Keys and values of any length, kept in heap pages after their leaves, after
// the Rust crate's `bbtree-mmap/src/var_bytes/tests.rs`: what a span's check
// refuses.
#include <bbtree_mmap/var_bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using bbtree_mmap::var_bytes;

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
