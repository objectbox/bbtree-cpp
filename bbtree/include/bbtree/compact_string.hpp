// `bbtree::compact_string` and `bbtree::compact_string_24`: owning byte
// strings of 16 and 24 bytes, for string keys of bbtree's maps (PORTING.md,
// P29). Both are `basic_compact_string<Size, ShortCapacity>`: a size of type
// `Size` and up to `ShortCapacity` bytes in place.
//
//   bbtree::unsync::bbtree_map<bbtree::compact_string, int, std::less<>> map;
//   map["short key"] = 1;                 // up to 12 bytes: no allocation
//   map.find(std::string("short key"));   // lookups take std::string,
//   map.find(std::string_view("..."));    // string_view, const char*
//   std::string_view key = map.begin()->first;
//
// Why not `std::string`: libstdc++'s is 32 bytes and points into itself while
// short, so a leaf holds fewer of them and shifts them one move at a time.
// These hold their short strings in themselves with no pointer to them, and
// move as bytes (`is_trivially_relocatable`), so a leaf shifts its keys with
// one memmove.
//
// Layout, `ShortCapacity + sizeof(Size)` bytes, 16 or 24; the size follows
// the bytes held in place, in either form, so reading it is one load (P29):
// - short (size <= ShortCapacity): bytes 0..ShortCapacity are the string,
//   zero after it;
// - long: bytes 0..8 point to `size + 1` heap bytes (the string and a zero).
// `data()` is not zero-terminated for a string of exactly `ShortCapacity`
// bytes, so there is no `c_str()`. A 2-byte size holds strings up to 65,535
// bytes; longer ones throw `std::length_error`.
//
// No prefix of the bytes is kept beside the pointer, as Umbra's "German"
// strings keep 4: bbtree's nodes keep their own 8-byte inline key of every
// key and compare a stored key's bytes only past it (P29), so a prefix would
// save no load.
#pragma once

#include <bbtree/inline_key.hpp>
#include <bbtree/relocation.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace bbtree {

template <class Size, std::size_t ShortCapacity>
class basic_compact_string {
  static_assert(std::is_same_v<Size, std::uint16_t> || std::is_same_v<Size, std::uint32_t>,
                "the size is a std::uint16_t or a std::uint32_t");
  static_assert(ShortCapacity + sizeof(Size) == 16 || ShortCapacity + sizeof(Size) == 24,
                "a compact string is 16 or 24 bytes");

 public:
  // The longest string held in place.
  static constexpr std::size_t short_capacity = ShortCapacity;
  // The longest string at all.
  static constexpr std::size_t max_size = std::numeric_limits<Size>::max();

  basic_compact_string() noexcept { set_short_size(0); }

  // From anything a string_view is made from: `std::string`, `string_view`,
  // `const char*`. Implicit, so that inserts and `operator[]` take them.
  template <class S>
    requires(std::convertible_to<const S&, std::string_view> && !std::same_as<S, basic_compact_string>)
  basic_compact_string(const S& text)
      : basic_compact_string(std::string_view(text)) {}  // NOLINT(google-explicit-constructor)

  explicit basic_compact_string(std::string_view text) { assign_bytes(text.data(), text.size()); }

  basic_compact_string(const basic_compact_string& other) { assign_bytes(other.data(), other.size()); }

  basic_compact_string(basic_compact_string&& other) noexcept {
    std::memcpy(bytes_, other.bytes_, sizeof bytes_);
    other.set_short_size(0);
  }

  basic_compact_string& operator=(const basic_compact_string& other) {
    if (this != &other) {
      basic_compact_string copy(other);
      swap(copy);
    }
    return *this;
  }

  basic_compact_string& operator=(basic_compact_string&& other) noexcept {
    basic_compact_string moved(std::move(other));
    swap(moved);
    return *this;
  }

  ~basic_compact_string() { release(); }

  void swap(basic_compact_string& other) noexcept {
    unsigned char held[sizeof bytes_];
    std::memcpy(held, bytes_, sizeof bytes_);
    std::memcpy(bytes_, other.bytes_, sizeof bytes_);
    std::memcpy(other.bytes_, held, sizeof bytes_);
  }

  // One load for either form: the walk over a map's entries reads it for
  // every key (P29).
  std::size_t size() const noexcept {
    Size size = 0;
    std::memcpy(&size, bytes_ + short_capacity, sizeof size);
    return size;
  }
  bool empty() const noexcept { return size() == 0; }
  const char* data() const noexcept {
    return size() > short_capacity ? heap_bytes() : reinterpret_cast<const char*>(bytes_);
  }
  std::string_view view() const noexcept { return {data(), size()}; }

  operator std::string_view() const noexcept { return view(); }  // NOLINT(google-explicit-constructor)
  explicit operator std::string() const { return std::string(view()); }

  // Comparisons with this type, and with anything a string_view is made from,
  // which match a `std::string` or a C string exactly: a `string_view`
  // overload alone would leave `a == b` of two compact strings ambiguous
  // between its normal and its reversed form.
  friend bool operator==(const basic_compact_string& a, const basic_compact_string& b) noexcept {
    return a.view() == b.view();
  }
  friend std::strong_ordering operator<=>(const basic_compact_string& a,
                                          const basic_compact_string& b) noexcept {
    return a.view() <=> b.view();
  }
  template <class S>
    requires(std::convertible_to<const S&, std::string_view> && !std::same_as<S, basic_compact_string>)
  friend bool operator==(const basic_compact_string& a, const S& b) noexcept {
    return a.view() == std::string_view(b);
  }
  template <class S>
    requires(std::convertible_to<const S&, std::string_view> && !std::same_as<S, basic_compact_string>)
  friend std::strong_ordering operator<=>(const basic_compact_string& a, const S& b) noexcept {
    return a.view() <=> std::string_view(b);
  }

 private:
  bool is_long() const noexcept { return size() > short_capacity; }

  void set_short_size(std::size_t size) noexcept {
    std::memset(bytes_, 0, sizeof bytes_);
    store_size(size);
  }

  void store_size(std::size_t size) noexcept {
    const auto stored = static_cast<Size>(size);
    std::memcpy(bytes_ + short_capacity, &stored, sizeof stored);
  }

  const char* heap_bytes() const noexcept {
    const char* heap = nullptr;
    std::memcpy(&heap, bytes_, sizeof heap);
    return heap;
  }

  // Makes this a copy of `count` bytes at `text`; this holds nothing yet.
  void assign_bytes(const char* text, std::size_t count) {
    if (count <= short_capacity) {
      set_short_size(count);
      if (count > 0) {
        std::memcpy(bytes_, text, count);
      }
      return;
    }
    if (count > max_size) {
      throw std::length_error("bbtree::basic_compact_string: too long for its size type");
    }
    char* heap = new char[count + 1];
    std::memcpy(heap, text, count);
    heap[count] = '\0';
    std::memset(bytes_, 0, sizeof bytes_);
    std::memcpy(bytes_, &heap, sizeof heap);
    store_size(count);
  }

  void release() noexcept {
    if (is_long()) {
      delete[] heap_bytes();
    }
  }

  alignas(8) unsigned char bytes_[ShortCapacity + sizeof(Size)];
};

// 16 bytes, 12 in place, strings of any length up to 4 GiB.
using compact_string = basic_compact_string<std::uint32_t, 12>;
// 24 bytes, 20 in place: for keys mostly past 12 bytes, as crate names
// with versions are.
using compact_string_24 = basic_compact_string<std::uint32_t, 20>;

static_assert(sizeof(compact_string) == 16);
static_assert(sizeof(compact_string_24) == 24);

// Its inline keys are windows of its bytes, as std::string's are.
template <class Size, std::size_t ShortCapacity>
struct byte_string<basic_compact_string<Size, ShortCapacity>> {
  static constexpr bool holds_its_bytes = false;
  static std::span<const std::uint8_t> bytes(const basic_compact_string<Size, ShortCapacity>& s) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
  }
};

// No pointer into itself: it moves as bytes.
template <class Size, std::size_t ShortCapacity>
struct is_trivially_relocatable<basic_compact_string<Size, ShortCapacity>> : std::true_type {};

}  // namespace bbtree

template <class Size, std::size_t ShortCapacity>
struct std::hash<bbtree::basic_compact_string<Size, ShortCapacity>> {
  std::size_t operator()(const bbtree::basic_compact_string<Size, ShortCapacity>& s) const noexcept {
    return std::hash<std::string_view>{}(s.view());
  }
};
