// Keys and values of any length in a map file (15-index-file.md): a leaf keeps
// an 8-byte span for each, and their bytes lie in the heap pages after the
// leaf (`heap_pages.hpp`).
//
// - `var_bytes` is the span: `{len, offset}`, its bytes `offset` bytes past the
//   span itself. It orders as its bytes do and has their inline keys, so the
//   map's search, which hands a key's specializations only the key, reads
//   them without knowing where the node is.
// - `var_bytes_query` is what a lookup compares spans with: a span marked by a
//   length no span has, followed by the caller's bytes' address, so a lookup
//   copies nothing.
// - `arena_bytes` is what a writer's tree holds instead while it builds: a
//   pointer to a length-prefixed record in a `byte_arena`, of the span's size,
//   alignment and inline keys, so the writer's pages are a file map's but for
//   those slots, which the heap pages' writer rewrites into spans.
#pragma once

#include <bbtree/file.hpp>
#include <bbtree/inline_key.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace bbtree_mmap {

class alignas(8) var_bytes {
 public:
  // The length that marks a query (`var_bytes_query`), which no span has.
  static constexpr std::uint32_t query_len = 0xffffffff;

  static void encode(std::byte* slot, std::uint32_t len, std::uint32_t offset) noexcept {
    std::memcpy(slot, &len, 4);
    std::memcpy(slot + 4, &offset, 4);
  }

  // The bytes the span covers, or a query's caller-owned bytes.
  std::span<const std::uint8_t> bytes() const noexcept;

  friend std::strong_ordering operator<=>(const var_bytes& a, const var_bytes& b) noexcept {
    return bbtree::detail::search_bytes(a.bytes()).cmp_from(b.bytes(), 0);
  }
  friend bool operator==(const var_bytes& a, const var_bytes& b) noexcept { return (a <=> b) == 0; }

 private:
  friend class var_bytes_query;
  constexpr var_bytes(std::uint32_t len, std::uint32_t offset) noexcept : len_(len), offset_(offset) {}

  std::uint32_t len_;
  std::uint32_t offset_;
};

// A lookup's key: a span marked as a query, then the bytes it stands for.
class var_bytes_query {
 public:
  explicit var_bytes_query(std::span<const std::uint8_t> bytes) noexcept
      : span_(var_bytes::query_len, 0), data_(bytes.data()), size_(bytes.size()) {}
  explicit var_bytes_query(std::string_view text) noexcept
      : var_bytes_query(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())) {}
  var_bytes_query(const var_bytes_query&) = delete;
  var_bytes_query& operator=(const var_bytes_query&) = delete;

  // The query as a span, which the map's search compares as a key.
  const var_bytes& span() const noexcept { return span_; }

 private:
  friend class var_bytes;
  var_bytes span_;
  const std::uint8_t* data_;
  std::size_t size_;
};

inline std::span<const std::uint8_t> var_bytes::bytes() const noexcept {
  if (len_ == query_len) {
    // A query's span is its first member: the query is at its address.
    const auto* query = reinterpret_cast<const var_bytes_query*>(this);
    return {query->data_, query->size_};
  }
  return {reinterpret_cast<const std::uint8_t*>(this) + offset_, len_};
}

// A writer's key or value: a pointer to its record, `[u32 len][bytes]`.
class alignas(8) arena_bytes {
 public:
  explicit arena_bytes(const std::uint8_t* record) noexcept : record_(record) {}

  std::span<const std::uint8_t> bytes() const noexcept {
    std::uint32_t len;
    std::memcpy(&len, record_, 4);
    return {record_ + 4, len};
  }

  friend std::strong_ordering operator<=>(const arena_bytes& a, const arena_bytes& b) noexcept {
    return bbtree::detail::search_bytes(a.bytes()).cmp_from(b.bytes(), 0);
  }
  friend bool operator==(const arena_bytes& a, const arena_bytes& b) noexcept { return (a <=> b) == 0; }

 private:
  const std::uint8_t* record_;
};
static_assert(sizeof(arena_bytes) == sizeof(var_bytes) && alignof(arena_bytes) == alignof(var_bytes));

// Where a writer keeps its records until the file is written.
class byte_arena {
 public:
  arena_bytes add(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= var_bytes::query_len) {
      throw std::length_error("bbtree_mmap: a key or value of 4 GiB or more");
    }
    auto record = std::make_unique<std::uint8_t[]>(bytes.size() + 4);
    const auto len = static_cast<std::uint32_t>(bytes.size());
    std::memcpy(record.get(), &len, 4);
    if (!bytes.empty()) {
      std::memcpy(record.get() + 4, bytes.data(), bytes.size());
    }
    records_.push_back(std::move(record));
    return arena_bytes(records_.back().get());
  }

 private:
  std::vector<std::unique_ptr<std::uint8_t[]>> records_;
};

}  // namespace bbtree_mmap

// Both have the inline keys of their bytes, with hash tags and comparisons
// past the bytes known equal, as std byte strings do (D36, D43).
template <class Bytes>
  requires(std::is_same_v<Bytes, bbtree_mmap::var_bytes> || std::is_same_v<Bytes, bbtree_mmap::arena_bytes>)
struct bbtree::inline_key<Bytes> {
  static bbtree::inline_word at(const Bytes& key, std::size_t offset) noexcept {
    return bbtree::detail::big_endian::window(key.bytes(), offset);
  }
  static std::uint8_t hash_tag(const Bytes& key) noexcept {
    const std::size_t len = key.bytes().size();
    return len < 255 ? static_cast<std::uint8_t>(len) : std::uint8_t{255};
  }
  static std::strong_ordering cmp_from(const Bytes& a, const Bytes& b, std::size_t equal_len) noexcept {
    return bbtree::detail::search_bytes(a.bytes()).cmp_from(b.bytes(), equal_len);
  }
  static std::strong_ordering cmp_prefix(const Bytes& key, std::span<const std::uint8_t> prefix) noexcept {
    return bbtree::detail::search_bytes(key.bytes()).cmp_prefix(prefix);
  }
};

// A span keeps its bytes after its leaf; a writer's tree holds arena records.
template <>
struct bbtree::file_storage<bbtree_mmap::var_bytes> {
  static constexpr bool has_bytes = true;
  static constexpr std::uint64_t format = 1;
  using in_writer = bbtree_mmap::arena_bytes;

  static void check(std::span<const std::byte> file_bytes, std::size_t slot) {
    if (slot + 8 > file_bytes.size()) {
      throw bbtree::file_error(bbtree::file_error::kind::corrupt, "a span outside the file map");
    }
    std::uint32_t len;
    std::uint32_t offset;
    std::memcpy(&len, file_bytes.data() + slot, 4);
    std::memcpy(&offset, file_bytes.data() + slot + 4, 4);
    if (len == bbtree_mmap::var_bytes::query_len ||
        static_cast<std::uint64_t>(slot) + offset + len > file_bytes.size()) {
      throw bbtree::file_error(bbtree::file_error::kind::corrupt,
                               "a key or value's bytes outside the file map");
    }
  }
};
