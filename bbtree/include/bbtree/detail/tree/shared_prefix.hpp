// What every key of a tree shares, from its first byte (D50): the prefix of
// the nodes along the tree's borders, which lack a fence on one side, and of
// its root, which lacks both. A search checks a query against it once, at the
// top, so that every node may skip its prefix unchecked. The tree keeps it
// beside its root page pointer.
#pragma once

#include <bbtree/detail/node/tree_types.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace bbtree::detail {

// Invariant: the encoding of every key of the tree starts with these bytes,
// and a byte-string key is at least as long; every border node has this
// prefix length.
class shared_prefix {
 public:
  // The most bytes a tree keeps: what fits beside the length in 40 bytes.
  static constexpr std::size_t max_len = 39;

  constexpr shared_prefix() noexcept = default;

  // What a tree of `key` alone shares: its encoding's first bytes.
  template <class T>
  static shared_prefix of_key(const typename T::key_type& key) noexcept {
    shared_prefix shared;
    shared.len_ = static_cast<std::uint8_t>(T::inliner::write_leading_bytes(key, shared.bytes_));
    return shared;
  }

  std::span<const std::uint8_t> bytes() const noexcept { return {bytes_.data(), len_}; }
  std::size_t len() const noexcept { return len_; }

  // How many of these bytes `key`'s encoding starts with.
  template <class T>
  std::size_t len_shared_with(const typename T::key_type& key) const noexcept {
    return len_ == 0 ? 0 : T::inliner::common_prefix_len_with_bytes(key, bytes());
  }

  template <class T>
  bool is_shared_by(const typename T::key_type& key) const noexcept {
    return len_shared_with<T>(key) == len_;
  }

  void shrink_to(std::size_t len) noexcept {
    if (len < len_) {
      len_ = static_cast<std::uint8_t>(len);
    }
  }

  // The window of these bytes at `prefix_len`: what a border node whose
  // prefix shrinks to it fills its windows from.
  window window_at(std::size_t prefix_len) const noexcept {
    return {prefix_len, big_endian::window(bytes(), prefix_len)};
  }

  // Where `key` sorts against every key of the tree if it lacks these bytes:
  // before or after them all. None if it has them, or if the nodes compare
  // full keys only for it.
  template <class T, class Q>
  std::optional<std::strong_ordering> place_outside(const search_key<T, Q>& key) const noexcept {
    if constexpr (!search_key<T, Q>::compares_inline) {
      return std::nullopt;
    } else {
      if (len_ == 0) {
        return std::nullopt;
      }
      const std::strong_ordering ordering = key.cmp_shared_prefix(bytes());
      if (ordering == 0) {
        return std::nullopt;
      }
      return ordering;
    }
  }

  friend bool operator==(const shared_prefix& a, const shared_prefix& b) noexcept {
    return a.len_ == b.len_ && std::equal(a.bytes_.begin(), a.bytes_.begin() + a.len_, b.bytes_.begin());
  }

 private:
  std::uint8_t len_ = 0;
  std::array<std::uint8_t, max_len> bytes_{};
};

// A key that shares what every key of its tree shares (D50); only the tree
// makes one, by shrinking what its keys share or checking it. It refers to
// the caller's key, of reference type `KeyRef` (`const K&` or `K&&`), which
// it copies or moves only as the key goes into a node: an insert that finds
// the key leaves the caller's key as it was, as `std::map::try_emplace` does.
template <class KeyRef>
class prefixed_key {
  using K = std::remove_cvref_t<KeyRef>;

 public:
  explicit prefixed_key(KeyRef key) noexcept : key_(&key) {}

  const K& key() const noexcept { return *key_; }

  // The key with its window at the prefix length of the node it goes into.
  template <class T>
  windowed_key<K> window_at(std::size_t prefix_len) const
      noexcept(std::is_nothrow_constructible_v<K, KeyRef>) {
    const inline_word inline_key = T::inliner::inline_key_at(*key_, prefix_len);
    return {K(static_cast<KeyRef>(*key_)), window(prefix_len, inline_key)};
  }

 private:
  std::remove_reference_t<KeyRef>* key_;
};

}  // namespace bbtree::detail
