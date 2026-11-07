// Inline keys (D18): a few bytes of each key, kept in the node beside it, that
// order like the key. A search compares them first, in vectors, and reads a
// key only where they tie.
//
// A key's inline key at `offset` is bytes `offset..offset + 8` of an encoding
// that orders like the key, big-endian, zero-padded past its end: a *window*.
// A node keeps the first 2, 4 or 8 bytes of each window, past the bytes its
// fences share (05-fence-key.md).
//
// Which keys have inline keys is decided at compile time by specialization:
// - std byte strings, `byte_string<K>`: `std::string`, `std::string_view`,
//   `std::vector<std::uint8_t>`, and the byte arrays `std::array<std::uint8_t,
//   N>` for N in 16, 20, 32 (D18, D32);
// - the 64- and 128-bit integers, `inline_key<K>` (D31);
// - any type whose user specializes `inline_key<K>`.
// The Rust crate needed two strategies (`Auto`, `WithInlineKey`) and type
// identity tricks for this, as Rust has no specialization (PORTING.md, P5).
#pragma once

#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bbtree {

// The 128-bit integers, a GCC and Clang extension.
__extension__ typedef __int128 int128;
__extension__ typedef unsigned __int128 uint128;

// A window: 8 bytes of a key's encoding, big-endian.
using inline_word = std::uint64_t;

// How many bytes of each window a node keeps.
enum class inline_width : std::uint8_t { two = 2, four = 4, eight = 8 };

// The customization point: specialize it for a key type to give the key
// inline keys. A specialization has
//
//   static inline_word at(const K& key, std::size_t offset) noexcept;
//     bytes offset..offset + 8 of an encoding that orders like K's operator<,
//     zero-padded. Two keys whose windows at an offset where their encodings
//     agree before it differ must order as the windows do.
//
// and optionally
//
//   static constexpr inline_width width;  // bytes a node keeps; default eight
//   static std::uint8_t hash_tag(const K&) noexcept;  // equal keys, equal tags (D36)
//   static std::strong_ordering cmp_from(const K& a, const K& b,
//                                        std::size_t equal_len) noexcept;
//     how a orders against b, given their first equal_len encoded bytes
//     are equal (D43)
//   static std::strong_ordering cmp_prefix(const K&, std::span<const std::uint8_t>) noexcept;
//     how the key's zero-padded encoding orders against a prefix in its first
//     prefix.size() bytes (D50); by default compared a window at a time.
//
// None of them may throw (D48).
template <class K>
struct inline_key;

namespace detail {

// Big-endian word loads and the zero-padded window of a byte string.
struct big_endian {
  static inline_word load(const std::uint8_t* bytes) noexcept {
    inline_word word;
    std::memcpy(&word, bytes, sizeof word);
    if constexpr (std::endian::native == std::endian::little) {
      word = __builtin_bswap64(word);
    }
    return word;
  }

  // Bytes `offset..offset + 8` of `bytes`, zero-padded past their end.
  static inline_word window(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    const std::size_t len = bytes.size();
    if (len >= sizeof(inline_word)) {
      const std::size_t last_start = len - sizeof(inline_word);
      if (offset <= last_start) {
        return load(bytes.data() + offset);
      }
      const std::size_t shift = offset - last_start;
      return shift >= sizeof(inline_word) ? 0 : load(bytes.data() + last_start) << (8 * shift);
    }
    if (offset >= len) {
      return 0;
    }
    return short_tail(bytes.data() + offset, len - offset);
  }

 private:
  // The `count` bytes at `bytes` (1 to 7), big-endian at the word's top and
  // zero-padded: two fixed-size loads from either end, which overlap, so
  // that no load reads past the bytes, in place of a copy of `count` bytes,
  // which compilers make a call to `memcpy` (the idea is stx_btree's).
  static inline_word short_tail(const std::uint8_t* bytes, std::size_t count) noexcept {
    const auto shift = static_cast<unsigned>(64 - 8 * count);
    if (count >= 4) {
      const inline_word first = load_u32(bytes);
      const inline_word last = load_u32(bytes + count - 4);
      return (first << 32) | (last << shift);
    }
    if (count >= 2) {
      const inline_word first = load_u16(bytes);
      const inline_word last = load_u16(bytes + count - 2);
      return (first << 48) | (last << shift);
    }
    return inline_word{bytes[0]} << 56;
  }

  static inline_word load_u32(const std::uint8_t* bytes) noexcept {
    std::uint32_t word;
    std::memcpy(&word, bytes, sizeof word);
    if constexpr (std::endian::native == std::endian::little) {
      word = __builtin_bswap32(word);
    }
    return word;
  }

  static inline_word load_u16(const std::uint8_t* bytes) noexcept {
    std::uint16_t word;
    std::memcpy(&word, bytes, sizeof word);
    if constexpr (std::endian::native == std::endian::little) {
      word = __builtin_bswap16(word);
    }
    return word;
  }
};

// The bytes of a std byte string being searched for or compared (the Rust
// crate's `SearchBytes`): windows, and comparisons against a stored string,
// a prefix, and what every key of a tree shares.
class search_bytes {
 public:
  constexpr search_bytes() noexcept = default;
  constexpr explicit search_bytes(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

  std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }

  inline_word inline_key_at(std::size_t offset) const noexcept { return big_endian::window(bytes_, offset); }

  // How the bytes, zero-padded, order against `prefix` in its first
  // prefix.size() bytes: equal if they have them, or end inside them with
  // only zeros there. Whole 32-byte chunks first test equality.
  std::strong_ordering cmp_prefix(std::span<const std::uint8_t> prefix) const noexcept {
    constexpr std::size_t chunk = 32;
    const std::size_t end = prefix.size();
    std::size_t offset = 0;
    while (end - offset >= chunk && offset + chunk <= bytes_.size() &&
           std::memcmp(bytes_.data() + offset, prefix.data() + offset, chunk) == 0) {
      offset += chunk;
    }
    while (offset < end) {
      const std::size_t remaining = end - offset;
      const unsigned dropped_bits = remaining >= 8 ? 0 : static_cast<unsigned>(8 * (8 - remaining));
      const inline_word query = inline_key_at(offset) >> dropped_bits;
      const inline_word shared = big_endian::window(prefix, offset) >> dropped_bits;
      if (query != shared) {
        return query <=> shared;
      }
      offset += 8;
    }
    return std::strong_ordering::equal;
  }

  // How the bytes order against `shared`, which every key of a tree starts
  // with (D50): as `cmp_prefix`, but bytes ending inside it go first.
  std::strong_ordering cmp_shared(std::span<const std::uint8_t> shared) const noexcept {
    const std::strong_ordering ordering = cmp_prefix(shared);
    if (ordering == 0 && bytes_.size() < shared.size()) {
      return std::strong_ordering::less;
    }
    return ordering;
  }

  // How the bytes order against `stored` lexicographically, given that their
  // first `equal_len` bytes, zero-padded, are equal: 8 bytes at a time from
  // there, then by length. The last word ends where the shorter string does.
  std::strong_ordering cmp_from(std::span<const std::uint8_t> stored, std::size_t equal_len) const noexcept {
    const std::size_t both_len = bytes_.size() < stored.size() ? bytes_.size() : stored.size();
    if (equal_len < both_len && both_len < 8) {
      const inline_word query = inline_key_at(0);
      const inline_word other = big_endian::window(stored, 0);
      if (query != other) {
        return query <=> other;
      }
      return bytes_.size() <=> stored.size();
    }
    std::size_t compared = equal_len;
    while (compared < both_len) {
      const std::size_t word_end = compared + 8 < both_len ? compared + 8 : both_len;
      const inline_word query = big_endian::load(bytes_.data() + word_end - 8);
      const inline_word other = big_endian::load(stored.data() + word_end - 8);
      if (query != other) {
        return query <=> other;
      }
      compared = word_end;
    }
    return bytes_.size() <=> stored.size();
  }

 private:
  std::span<const std::uint8_t> bytes_;
};

}  // namespace detail

// The std byte strings whose inline keys are windows of their bytes. A
// specialization has `bytes(const T&)` and `holds_its_bytes`: whether the
// value is its bytes (an array), so a tie costs no read past the node.
template <class T>
struct byte_string;

template <>
struct byte_string<std::string> {
  static constexpr bool holds_its_bytes = false;
  static std::span<const std::uint8_t> bytes(const std::string& s) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
  }
};

template <>
struct byte_string<std::string_view> {
  static constexpr bool holds_its_bytes = false;
  static std::span<const std::uint8_t> bytes(std::string_view s) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
  }
};

template <>
struct byte_string<std::vector<std::uint8_t>> {
  static constexpr bool holds_its_bytes = false;
  static std::span<const std::uint8_t> bytes(const std::vector<std::uint8_t>& v) noexcept {
    return {v.data(), v.size()};
  }
};

template <std::size_t N>
  requires(N == 16 || N == 20 || N == 32)
struct byte_string<std::array<std::uint8_t, N>> {
  static constexpr bool holds_its_bytes = true;
  static std::span<const std::uint8_t> bytes(const std::array<std::uint8_t, N>& a) noexcept {
    return {a.data(), N};
  }
};

// A string literal or C string, as a query only: maps of std::string keys
// with a transparent comparator look it up by its bytes.
template <>
struct byte_string<const char*> {
  static constexpr bool holds_its_bytes = false;
  static std::span<const std::uint8_t> bytes(const char* s) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)};
  }
};

template <class T>
concept std_byte_string = requires(const T& t) {
                            { byte_string<T>::bytes(t) } -> std::same_as<std::span<const std::uint8_t>>;
                          };

// Byte spans: the windows of their bytes, as std byte strings have (the Rust
// crate's `InlineKey for [u8]`), for a key that is a byte string by another
// name to forward to, as bbtree-store's keys do. Hash tags are lengths,
// capped at 255.
template <>
struct inline_key<std::span<const std::uint8_t>> {
  using bytes_type = std::span<const std::uint8_t>;

  static inline_word at(bytes_type key, std::size_t offset) noexcept {
    return detail::big_endian::window(key, offset);
  }
  static std::uint8_t hash_tag(bytes_type key) noexcept {
    return key.size() < 255 ? static_cast<std::uint8_t>(key.size()) : std::uint8_t{255};
  }
  static std::strong_ordering cmp_from(bytes_type a, bytes_type b, std::size_t equal_len) noexcept {
    return detail::search_bytes(a).cmp_from(b, equal_len);
  }
  static std::strong_ordering cmp_prefix(bytes_type key, bytes_type prefix) noexcept {
    return detail::search_bytes(key).cmp_prefix(prefix);
  }
};

// Unsigned integers: their big-endian bytes (D31); 2-byte inline keys.
template <class T>
  requires(std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) == 8)
struct inline_key<T> {
  static constexpr inline_width width = inline_width::two;
  static inline_word at(T key, std::size_t offset) noexcept {
    return offset < 8 ? static_cast<inline_word>(key) << (8 * offset) : 0;
  }
};

// Signed integers: with the sign bit flipped, which orders like them.
template <class T>
  requires(std::is_integral_v<T> && std::is_signed_v<T> && sizeof(T) == 8)
struct inline_key<T> {
  static constexpr inline_width width = inline_width::two;
  static inline_word at(T key, std::size_t offset) noexcept {
    const auto flipped = static_cast<std::uint64_t>(key) ^ (std::uint64_t{1} << 63);
    return offset < 8 ? flipped << (8 * offset) : 0;
  }
};

template <>
struct inline_key<uint128> {
  static constexpr inline_width width = inline_width::two;
  static inline_word at(uint128 key, std::size_t offset) noexcept {
    return offset < 16 ? static_cast<inline_word>((key << (8 * offset)) >> 64) : 0;
  }
};

template <>
struct inline_key<int128> {
  static constexpr inline_width width = inline_width::two;
  static inline_word at(int128 key, std::size_t offset) noexcept {
    const auto flipped = static_cast<uint128>(key) ^ (static_cast<uint128>(1) << 127);
    return inline_key<uint128>::at(flipped, offset);
  }
};

// Whether `K` has an `inline_key` specialization.
template <class K>
concept has_inline_key = requires(const K& key) {
                           { inline_key<K>::at(key, std::size_t{}) } -> std::same_as<inline_word>;
                         };

}  // namespace bbtree
