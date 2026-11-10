// What a map file keeps after each leaf: the bytes of its keys and values of
// any length, in heap pages, as a store's leaf keeps its records
// (15-index-file.md, D58).
//
// - Heap pages are the node config's heap page size (the leaves' by default),
//   right after their leaf, so that a lookup reads the leaf and its bytes from
//   one place of the file.
// - A record is an entry's key's bytes, then its value's, whichever of them a
//   span keeps, in key order, the head's first.
// - A record stays in one page, starting the next if it doesn't fit what is
//   left of one; one larger than a page starts on a page and runs on.
#pragma once

#include <bbtree_mmap/var_bytes.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace bbtree_mmap {

struct heap_pages {
  template <class Leaf>
  static std::size_t len(const Leaf& leaf) {
    return lay_out(leaf, [](std::size_t, std::size_t, std::span<const std::uint8_t>) {});
  }

  // Writes the leaf's records into `bytes`, right after the leaf's copy at
  // `page`, and rewrites the copy's slots into spans to them.
  template <class Leaf>
  static void write(const Leaf& leaf, std::byte* page, std::size_t page_size, std::byte* bytes) {
    lay_out(leaf, [&](std::size_t slot, std::size_t at, std::span<const std::uint8_t> record) {
      const std::uint64_t offset = page_size + at - slot;
      if (offset >= var_bytes::query_len) {
        throw std::length_error("bbtree_mmap: a leaf's heap pages past 4 GiB of its slots");
      }
      var_bytes::encode(page + slot, static_cast<std::uint32_t>(record.size()),
                        static_cast<std::uint32_t>(offset));
      if (!record.empty()) {
        std::memcpy(bytes + at, record.data(), record.size());
      }
    });
  }

 private:
  template <class T>
  static std::span<const std::uint8_t> bytes_of(const T& slot) noexcept {
    if constexpr (std::is_same_v<T, arena_bytes>) {
      return slot.bytes();
    } else {
      return {};
    }
  }

  // Places every record of the leaf, calling `place(slot, at, bytes)` for
  // each key or value with bytes; returns the heap pages' length.
  template <class Leaf, class Place>
  static std::size_t lay_out(const Leaf& leaf, Place&& place) {
    using T = typename Leaf::types;
    using K = typename T::key_type;
    using V = typename T::mapped_type;
    constexpr bool keys_have_bytes = std::is_same_v<K, arena_bytes>;
    constexpr bool values_have_bytes = std::is_same_v<V, arena_bytes>;
    constexpr std::size_t page = bbtree::detail::heap_page_size_of<typename T::config>;
    constexpr auto layout = T::leaf;
    std::size_t top = 0;
    auto entry = [&](std::size_t key_slot, std::span<const std::uint8_t> key_bytes, std::size_t value_slot,
                     const V& value) {
      const auto value_bytes = bytes_of(value);
      const std::size_t size = key_bytes.size() + value_bytes.size();
      if (size > page - top % page) {
        top = (top + page - 1) / page * page;
      }
      if constexpr (keys_have_bytes) {
        place(key_slot, top, key_bytes);
      }
      if constexpr (values_have_bytes) {
        place(value_slot, top + key_bytes.size(), value_bytes);
      }
      top += size;
    };
    if constexpr (keys_have_bytes || values_have_bytes) {
      if (leaf.has_head()) {
        // The head is the entry of the separator left of the leaf (D17).
        // Without inline keys its key stays in the internal node, and the
        // leaf has its value alone.
        if constexpr (T::has_inline_keys) {
          entry(*layout.head_key_offset, bytes_of(leaf.head_key()), layout.head_offset, leaf.head_value());
        } else {
          entry(0, {}, layout.head_offset, leaf.head_value());
        }
      }
      for (std::size_t index = 0; index < leaf.len(); ++index) {
        entry(layout.keys.keys_offset + index * layout.keys.key_stride, bytes_of(leaf.key(index)),
              layout.values_offset() + index * layout.value_stride, leaf.value(index));
      }
    }
    return (top + page - 1) / page * page;
  }
};

}  // namespace bbtree_mmap
