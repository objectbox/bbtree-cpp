// Compile-time byte layout of the node pages of each kind, without and with
// inline keys of each width (D9, D12, D13, D15, D17, D21, D29, D34, D36, D49).
//
//   leaf      | header | k0 k1 … k_n | head v0 v1 … v_n |
//   internal  | header | k0 k1 … k_n | e0 e1 … e_n+1     |
//
//   with inline keys:
//   leaf      | header | w0 … w_n | (hk head) (k0 v0) (k1 v1) … (k_n v_n) |
//   internal  | header | w0 … w_n | e0 e1 … e_n+1                         |
//
// - Every layout has a header at offset 0, and one more trailing slot than
//   keys: a leaf's head and values, or an internal node's edges. Trailing
//   slot 0 is the head, or edge 0; slot i + 1 goes with key i.
// - Without inline keys the keys are an array of their own, at the same
//   offset in both kinds, and the trailing slots an array after them.
// - With inline keys, `capacity` inline keys follow the header from offset 8
//   (16 in a latched tree). Leaves pair each key with its value in an entry;
//   entry 0 holds the head's key and value (D49). Power-of-two entries up to
//   a cache line start at a multiple of their size (D34). Internal nodes keep
//   no keys, only inline keys and edges.
// - A leaf whose plan keeps hash tags has a byte per key after its inline
//   keys, where the layout has room (D36).
// - Each kind is the smallest multiple of its page size that holds 12 keys,
//   aligned to it.
//
// Every invariant the node code relies on is checked when a layout is
// computed, which happens at compile time.
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/inline_key.hpp>
#include <bbtree/node_config.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace bbtree::detail {

// The header at the start of every node page.
struct node_header {
  std::uint16_t len;
  // Whether a leaf holds a head (D17); its head slot is initialized exactly
  // when this is set, and so is its head key slot with inline keys (D49).
  bool has_head;
};

// The header of a node with inline keys: the plain header, then how many
// leading bytes the node's fences share, which its inline keys skip.
struct inline_key_node_header {
  node_header header;
  std::uint16_t prefix_len;

  static constexpr std::size_t max_prefix_len = 0xffff;
};

// A latched tree's node header: the inline-key header, then the latch word
// (09-concurrency.md), at offset 8.
struct latched_node_header_layout {
  static constexpr std::size_t latch_offset = 8;
  static constexpr std::size_t size = 16;
  static constexpr std::size_t align = 8;
};

// The bytes a slot of `T` takes: none for an empty type, as Rust's zero-sized
// types (a set's values).
template <class T>
inline constexpr std::size_t slot_size = std::is_empty_v<T> ? 0 : sizeof(T);

// Where the keys of one kind of node sit, and how many fit.
struct key_area {
  std::size_t capacity;
  // Where `capacity` inline keys start, in a layout with inline keys.
  std::optional<std::size_t> inline_keys_offset;
  // Where key 0 is; in an internal node that keeps no keys, where they would.
  std::size_t keys_offset;
  // Bytes from one key to the next: a key's size, or an entry's (D29); 0 in
  // an internal node that keeps no keys.
  std::size_t key_stride;

  friend constexpr bool operator==(const key_area&, const key_area&) = default;
};

struct leaf_layout {
  page_layout page;
  key_area keys;
  // The head, trailing slot 0; value i is trailing slot i + 1.
  std::size_t head_offset;
  std::size_t value_stride;
  std::optional<std::size_t> hash_tags_offset;
  // The head's key, in entry 0 before the head, with inline keys (D49).
  std::optional<std::size_t> head_key_offset;

  constexpr std::size_t values_offset() const { return head_offset + value_stride; }
  friend constexpr bool operator==(const leaf_layout&, const leaf_layout&) = default;
};

struct internal_layout {
  page_layout page;
  key_area keys;
  std::size_t edges_offset;
  std::size_t edge_stride;

  friend constexpr bool operator==(const internal_layout&, const internal_layout&) = default;
};

// Byte layouts of the node pages for keys `K` and values `V`, with the page
// sizes of config `C` and a leaf annex of type `Annex` (void: none).
template <class K, class V, class C, class Annex = void>
class node_layout {
 public:
  // The leaf layout without inline keys, or with inline keys of `width`; with
  // room for hash tags if `hash_tags` and they fit; latched if `latched`.
  static constexpr leaf_layout leaf(std::optional<inline_width> width, bool hash_tags, bool latched) {
    const header_shape header = leaf_header(latched);
    const std::size_t inline_key_size = inline_key_size_of(width);
    const kind_layout layout = compute_kind(leaf_page_size(), header, inline_key_size, 0, value_slot(), true);
    if (inline_key_size == 0 || !hash_tags) {
      return leaf_of(layout, std::nullopt);
    }
    const std::size_t tags_start = columns_end(header, inline_key_size, layout.keys.capacity);
    const std::size_t tags_end = tags_start + layout.keys.capacity;
    if (tags_end <= entries_offset(layout)) {
      return leaf_of(layout, tags_start);
    }
    if (tags_end <= layout.trailing_offset) {
      const kind_layout tagged =
          compute_kind(leaf_page_size(), header, inline_key_size, 1, value_slot(), true);
      return leaf_of(tagged, columns_end(header, inline_key_size, tagged.keys.capacity));
    }
    return leaf_of(layout, std::nullopt);
  }

  // The internal layout without inline keys, or with inline keys of `width`
  // and no keys (D49); latched if `latched`.
  static constexpr internal_layout internal(std::optional<inline_width> width, bool latched) {
    const std::size_t inline_key_size = inline_key_size_of(width);
    const bool keeps_keys = inline_key_size == 0;
    const std::size_t page_size = keeps_keys ? plain_internal_page_size() : internal_page_size();
    const header_shape header{latched, 0, 1};
    const kind_layout layout = compute_kind(page_size, header, inline_key_size, 0, edge_slot(), keeps_keys);
    return {layout.page, layout.keys, layout.trailing_offset, layout.trailing_stride};
  }

  // Where a leaf's annex is: right after its node header.
  static constexpr std::size_t annex_offset(bool latched) { return leaf_header(latched).annex_offset(); }

  static constexpr std::size_t cache_line_size = 64;
  static constexpr std::size_t min_capacity = 12;
  static constexpr std::size_t max_capacity = 1023;

 private:
  // A leaf's values or an internal node's edges: what follows each key.
  struct trailing_slot {
    std::size_t size;
    std::size_t align;
    bool pair_with_key;

    constexpr bool paired(std::size_t inline_key_size) const { return pair_with_key && inline_key_size > 0; }
  };

  // The node header's shape: latched or not, and a leaf's annex.
  struct header_shape {
    bool latched;
    std::size_t annex_size;
    std::size_t annex_align;

    constexpr std::size_t node_header_size() const {
      return latched ? latched_node_header_layout::size : sizeof(inline_key_node_header);
    }
    constexpr std::size_t annex_offset() const { return round_up(node_header_size(), annex_align); }
    constexpr std::size_t size() const {
      return annex_size == 0 ? node_header_size() : annex_offset() + annex_size;
    }
  };

  struct kind_layout {
    page_layout page;
    key_area keys;
    std::size_t trailing_offset;
    std::size_t trailing_stride;
  };

  static constexpr void require(bool condition, const char* what) {
    if (!condition) {
      throw what;  // A compile-time error where layouts are evaluated.
    }
  }

  static constexpr std::size_t round_up(std::size_t value, std::size_t align) {
    return (value + align - 1) / align * align;
  }
  static constexpr std::size_t max(std::size_t a, std::size_t b) { return a > b ? a : b; }
  static constexpr std::size_t min(std::size_t a, std::size_t b) { return a < b ? a : b; }
  static constexpr bool is_power_of_two(std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
  }

  static constexpr std::size_t checked_page_size(std::size_t size, std::size_t minimum) {
    require(is_power_of_two(size) && size >= minimum,
            "a node config page size is a power of two, at least its minimum");
    return size;
  }
  static constexpr std::size_t leaf_page_size() { return checked_page_size(C::leaf_page_size, 1024); }
  static constexpr std::size_t internal_page_size() {
    return checked_page_size(C::internal_page_size, cache_line_size);
  }
  static constexpr std::size_t plain_internal_page_size() {
    return checked_page_size(C::plain_internal_page_size, cache_line_size);
  }

  static constexpr std::size_t key_size = slot_size<K>;
  static constexpr std::size_t key_align = alignof(K);

  static constexpr trailing_slot value_slot() { return {slot_size<V>, alignof(V), true}; }
  static constexpr trailing_slot edge_slot() { return {sizeof(void*), alignof(void*), false}; }

  static constexpr header_shape leaf_header(bool latched) {
    if constexpr (std::is_void_v<Annex>) {
      return {latched, 0, 1};
    } else {
      return latched ? header_shape{true, slot_size<Annex>, alignof(Annex)} : header_shape{false, 0, 1};
    }
  }

  static constexpr std::size_t inline_key_size_of(std::optional<inline_width> width) {
    return width ? static_cast<std::size_t>(*width) : 0;
  }
  static constexpr std::size_t inline_keys_offset(header_shape header) {
    return round_up(header.size(), alignof(inline_word));
  }
  // Where the keys or entries could start: after the header and the arrays
  // beside the keys (`column_size` bytes per key).
  static constexpr std::size_t columns_end(header_shape header, std::size_t column_size,
                                           std::size_t capacity) {
    return column_size > 0 ? inline_keys_offset(header) + capacity * column_size : sizeof(node_header);
  }
  static constexpr std::size_t entry_trailing_offset(trailing_slot trailing) {
    return round_up(key_size, trailing.align);
  }
  static constexpr std::size_t entry_align(trailing_slot trailing) { return max(key_align, trailing.align); }
  static constexpr std::size_t entry_size(trailing_slot trailing) {
    return round_up(entry_trailing_offset(trailing) + trailing.size, entry_align(trailing));
  }
  static constexpr std::size_t entries_offset(kind_layout layout) {
    return layout.trailing_offset - entry_trailing_offset(value_slot());
  }

  static constexpr leaf_layout leaf_of(kind_layout layout, std::optional<std::size_t> hash_tags_offset) {
    std::optional<std::size_t> head_key_offset;
    if (layout.keys.inline_keys_offset) {
      head_key_offset = entries_offset(layout);
    }
    return {layout.page,      layout.keys,    layout.trailing_offset, layout.trailing_stride,
            hash_tags_offset, head_key_offset};
  }

  static constexpr std::size_t node_align(std::size_t page_size, std::size_t trailing_align,
                                          bool keeps_keys) {
    return max(page_size, max(keeps_keys ? key_align : 1, trailing_align));
  }

  // The fewest bytes a node of `capacity` keys needs.
  static constexpr std::size_t bytes_needed(header_shape header, std::size_t column_size,
                                            trailing_slot trailing, bool keeps_keys, std::size_t capacity) {
    const std::size_t keys_start = columns_end(header, column_size, capacity);
    if (trailing.paired(column_size)) {
      return round_up(keys_start, entry_align(trailing)) + (capacity + 1) * entry_size(trailing);
    }
    if (keeps_keys) {
      return round_up(round_up(keys_start, key_align) + capacity * key_size, trailing.align) +
             (capacity + 1) * trailing.size;
    }
    return round_up(keys_start, trailing.align) + (capacity + 1) * trailing.size;
  }

  // The most bytes inline keys may take: a page's or 1 KiB, whichever is more.
  static constexpr std::size_t max_inline_keys_size(std::size_t page_size) { return max(page_size, 1024); }

  static constexpr std::size_t max_capacity_of(std::size_t page_size, std::size_t inline_key_size) {
    return inline_key_size == 0 ? max_capacity
                                : min(max_capacity, max_inline_keys_size(page_size) / inline_key_size);
  }

  static constexpr std::size_t capacity_in(header_shape header, std::size_t node_size, std::size_t cap,
                                           std::size_t column_size, trailing_slot trailing, bool keeps_keys) {
    std::size_t size_per_key = column_size;
    if (trailing.paired(column_size)) {
      size_per_key += entry_size(trailing);
    } else if (keeps_keys) {
      size_per_key += key_size + trailing.size;
    } else {
      size_per_key += trailing.size;
    }
    std::size_t capacity = size_per_key == 0 ? cap : min(cap, node_size / size_per_key);
    while (bytes_needed(header, column_size, trailing, keeps_keys, capacity) > node_size) {
      --capacity;
    }
    return capacity;
  }

  // The smallest node, in steps of `page_size`, that holds `min_capacity`
  // keys, with as many keys as fit in it.
  static constexpr kind_layout compute_kind(std::size_t page_size, header_shape header,
                                            std::size_t inline_key_size, std::size_t hash_tag_size,
                                            trailing_slot trailing, bool keeps_keys) {
    const std::size_t column_size = inline_key_size + hash_tag_size;
    const std::size_t align = node_align(page_size, trailing.align, keeps_keys);
    const std::size_t size =
        round_up(bytes_needed(header, column_size, trailing, keeps_keys, min_capacity), align);
    const std::size_t capacity = capacity_in(header, size, max_capacity_of(page_size, inline_key_size),
                                             column_size, trailing, keeps_keys);
    const std::size_t keys_start = columns_end(header, column_size, capacity);
    std::size_t keys_offset = 0;
    std::size_t key_stride = 0;
    std::size_t trailing_offset = 0;
    std::size_t trailing_stride = 0;
    if (trailing.paired(inline_key_size)) {
      const std::size_t entry = entry_size(trailing);
      // The page and whole entries both take multiples of this alignment, so
      // rounding their start up only uses the page's tail (D34).
      const std::size_t entries_align =
          is_power_of_two(entry) && entry <= cache_line_size ? entry : entry_align(trailing);
      const std::size_t entries_start = round_up(keys_start, entries_align);
      keys_offset = entries_start + entry;
      key_stride = entry;
      trailing_offset = entries_start + entry_trailing_offset(trailing);
      trailing_stride = entry;
    } else if (keeps_keys) {
      keys_offset = round_up(keys_start, key_align);
      key_stride = key_size;
      trailing_offset = round_up(keys_offset + capacity * key_size, trailing.align);
      trailing_stride = trailing.size;
    } else {
      keys_offset = keys_start;
      trailing_offset = round_up(keys_start, trailing.align);
      trailing_stride = trailing.size;
    }
    std::optional<std::size_t> inline_keys_at;
    if (inline_key_size > 0) {
      inline_keys_at = inline_keys_offset(header);
    }
    const kind_layout layout{
        {size, align}, {capacity, inline_keys_at, keys_offset, key_stride}, trailing_offset, trailing_stride};
    check_invariants(layout, page_size, header, inline_key_size, column_size, trailing, keeps_keys);
    return layout;
  }

  // What every layout needs: whole pages, a capacity in bounds, and the
  // header, columns, keys and trailing slots in place, aligned, in the page
  // and clear of each other.
  static constexpr void check_invariants(kind_layout layout, std::size_t page_size, header_shape header,
                                         std::size_t inline_key_size, std::size_t column_size,
                                         trailing_slot trailing, bool keeps_keys) {
    const auto [size, align] = layout.page;
    const key_area& keys = layout.keys;
    require(is_power_of_two(align) && size % page_size == 0 && size % align == 0, "whole aligned pages");
    require(keys.capacity >= min_capacity && keys.capacity <= max_capacity, "capacity in bounds");
    const std::size_t keys_start = columns_end(header, column_size, keys.capacity);
    if (!keys.inline_keys_offset) {
      require(keys_start == sizeof(node_header) && !header.latched && header.annex_size == 0,
              "a plain header alone");
    } else {
      const std::size_t inline_keys_at = *keys.inline_keys_offset;
      require(inline_keys_at >= header.size() && inline_keys_at % alignof(inline_word) == 0,
              "inline keys after the header, aligned");
      require(align % alignof(inline_word) == 0, "nodes aligned for inline keys");
      require(keys.capacity * inline_key_size <= max_inline_keys_size(page_size),
              "inline keys fit their cap");
      require(inline_keys_at + keys.capacity * column_size == keys_start, "columns end where keys start");
      if (header.annex_size > 0) {
        require(align % header.annex_align == 0 && header.annex_offset() % header.annex_align == 0,
                "the annex aligned");
      }
    }
    require(align % trailing.align == 0 && layout.trailing_offset % trailing.align == 0,
            "trailing slots aligned");
    require(layout.trailing_stride >= trailing.size && layout.trailing_stride % trailing.align == 0,
            "trailing stride");
    if (keeps_keys) {
      require(align % key_align == 0 && keys.keys_offset % key_align == 0, "keys aligned");
      require(keys.key_stride >= key_size && keys.key_stride % key_align == 0, "key stride");
    } else {
      require(keys.key_stride == 0 && keys.inline_keys_offset.has_value(), "no keys without inline keys");
    }
    require(keys.keys_offset + keys.capacity * keys.key_stride <= size, "the last key ends in the page");
    require(layout.trailing_offset + keys.capacity * layout.trailing_stride + trailing.size <= size,
            "the last trailing slot ends in the page");
    if (trailing.paired(inline_key_size)) {
      const std::size_t slot_offset = entry_trailing_offset(trailing);
      const std::size_t entries_start = layout.trailing_offset - slot_offset;
      require(keys_start <= entries_start, "entries after the columns");
      require(keys.key_stride == entry_size(trailing) && layout.trailing_stride == keys.key_stride,
              "entries a stride apart");
      require(keys.keys_offset == entries_start + keys.key_stride, "key 0 in entry 1");
      require(slot_offset >= key_size && slot_offset + trailing.size <= keys.key_stride,
              "a slot after its key");
    } else if (keeps_keys) {
      require(keys_start <= keys.keys_offset &&
                  keys.keys_offset + keys.capacity * key_size <= layout.trailing_offset,
              "keys, then trailing slots");
    } else {
      require(keys_start <= layout.trailing_offset, "trailing slots after the inline keys");
    }
  }
};

}  // namespace bbtree::detail
