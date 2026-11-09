// A file map's bytes laid open for damage (D58, 15-index-file.md): the tests
// of file maps and the harness of checked opens (`file_map_open.hpp`) change
// a valid file map through it, a header word, a node's header, an edge or a
// slot at a time, and hand the result to a reader.
#pragma once

#include "fuzz_input.hpp"

#include <bbtree/file.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bbtree_fuzz {

// A node of a file map: where its page lies, its kind and its depth.
struct image_node {
  std::uint64_t offset;
  bool is_leaf;
  std::size_t depth;
};

template <class K, class V, class Compare = std::less<K>, class Strategy = bbtree::auto_inline_keys>
class file_image {
 public:
  using types = bbtree::detail::tree_types<K, V, Compare, bbtree::detail::in_file<Strategy>>;
  using reader = bbtree::file_map<K, V, Compare, Strategy>;

  // Where the header keeps its words (`detail::file_header`).
  static constexpr std::size_t length_slot = 24;
  static constexpr std::size_t root_slot = 32;
  static constexpr std::size_t byte_len_slot = 40;

  // A copy of `valid`, a file map as its writer wrote it, in a buffer of
  // exactly its size, so that AddressSanitizer sees a read past either end.
  explicit file_image(std::span<const std::byte> valid) : nodes_(nodes_of(valid)) { rebuffer(valid); }
  file_image(const file_image& other) : nodes_(other.nodes_) { rebuffer(other.bytes()); }
  file_image(file_image&&) noexcept = default;

  std::span<const std::byte> bytes() const noexcept { return buffer_.bytes(); }

  // The nodes of the file map as it was written, in level order: the root
  // first, the leaves last, each level left to right.
  const std::vector<image_node>& nodes() const noexcept { return nodes_; }

  std::vector<image_node> leaves() const {
    std::vector<image_node> found;
    for (const image_node& node : nodes_) {
      if (node.is_leaf) {
        found.push_back(node);
      }
    }
    return found;
  }

  // The last internal node at `depth`: the one whose last edge leads to the
  // last page of the level below, which no page is reached after.
  const image_node& last_internal_at(std::size_t depth) const {
    const image_node* last = nullptr;
    for (const image_node& node : nodes_) {
      if (!node.is_leaf && node.depth == depth) {
        last = &node;
      }
    }
    return *last;
  }

  // --- Where a node keeps things, as offsets into the file map ---

  static std::size_t len_slot(const image_node& node) noexcept {
    return node.offset + offsetof(bbtree::detail::node_header, len);
  }
  static std::size_t head_flag_slot(const image_node& node) noexcept {
    return node.offset + offsetof(bbtree::detail::node_header, has_head);
  }
  static std::size_t prefix_len_slot(const image_node& node) noexcept {
    return node.offset + offsetof(bbtree::detail::inline_key_node_header, prefix_len);
  }
  static std::size_t edge_slot(const image_node& node, std::size_t edge) noexcept {
    return node.offset + types::internal.edges_offset + edge * types::internal.edge_stride;
  }
  static std::size_t key_slot(const image_node& leaf, std::size_t index) noexcept {
    return leaf.offset + types::leaf.keys.keys_offset + index * types::leaf.keys.key_stride;
  }
  static std::size_t value_slot(const image_node& leaf, std::size_t index) noexcept {
    return leaf.offset + types::leaf.values_offset() + index * types::leaf.value_stride;
  }
  static std::size_t head_key_slot(const image_node& leaf) noexcept {
    return leaf.offset + *types::leaf.head_key_offset;
  }
  static std::size_t head_value_slot(const image_node& leaf) noexcept {
    return leaf.offset + types::leaf.head_offset;
  }
  static std::size_t capacity(const image_node& node) noexcept {
    return node.is_leaf ? types::leaf_capacity : types::internal_capacity;
  }
  static bbtree::page_layout page(const image_node& node) noexcept {
    return node.depth == 0 ? types::root_page : node.is_leaf ? types::leaf.page : types::internal.page;
  }

  // --- Reading and changing the bytes ---

  template <class Word>
  Word get(std::size_t at) const noexcept {
    Word word;
    std::memcpy(&word, buffer_.bytes().data() + at, sizeof word);
    return word;
  }
  template <class Word>
  void put(std::size_t at, Word word) noexcept {
    std::memcpy(buffer_.bytes().data() + at, &word, sizeof word);
  }

  std::size_t len(const image_node& node) const noexcept { return get<std::uint16_t>(len_slot(node)); }

  // Sets a leaf's length, and the header's count of entries to match, so
  // that the count is not what refuses the file map.
  void resize_leaf(const image_node& leaf, std::size_t new_len) noexcept {
    put(length_slot, get<std::uint64_t>(length_slot) + new_len - len(leaf));
    put(len_slot(leaf), static_cast<std::uint16_t>(new_len));
  }

  // Sets a leaf's head flag, and the header's count of entries to match.
  void set_head_flag(const image_node& leaf, std::uint8_t flag) noexcept {
    put(length_slot, get<std::uint64_t>(length_slot) + flag - get<std::uint8_t>(head_flag_slot(leaf)));
    put(head_flag_slot(leaf), flag);
  }

  // Why a checked open refuses the file map as corrupt; empty if it opens
  // it. Any other refusal is thrown on: the file map is of the reader's
  // layout.
  std::string corruption() const { return corruption_from(&reader::open); }
  // The same of an open that checks the header alone.
  std::string corruption_unchecked() const { return corruption_from(&reader::open_unchecked); }

  // The refusal of a copy that `damage` changed: the image stays as it is.
  template <class Damage>
  std::string corruption_after(Damage&& damage) const {
    file_image damaged(*this);
    damage(damaged);
    return damaged.corruption();
  }

  // Cuts the file map short after `keep` bytes.
  void truncate(std::size_t keep) { rebuffer(bytes().first(keep)); }

  // Damages the file map once, as `in` says: a byte anywhere, a header word,
  // its end, or a node's length, head flag, prefix length, edge or slot,
  // with the values a check of offsets and lengths must survive: zero, the
  // file map's size, and the last ones before a sum wraps.
  void damage(fuzz_input& in) {
    const std::size_t size = bytes().size();
    if (size < bbtree::detail::file_header::size) {
      return;  // cut down to less than a header: nothing is left to name
    }
    const std::uint8_t kind = in.byte() % 10;
    switch (kind) {
      case 0: {
        // Past the fingerprint, which refuses whatever else the bytes hold.
        const std::size_t at = length_slot + in.below(size - length_slot);
        put(at, in.byte());
        return;
      }
      case 1: {
        const std::size_t word = length_slot + 8 * in.below(3);
        put(word, extreme(in, types::root_page.align));
        return;
      }
      case 2: {
        const std::size_t keep = in.below(size + 1);
        if (in.byte() % 2 == 0 && keep >= bbtree::detail::file_header::size) {
          put(byte_len_slot, static_cast<std::uint64_t>(keep));
        }
        truncate(keep);
        return;
      }
      default:
        break;
    }
    if (nodes_.empty()) {
      return;
    }
    // The last internal node of a level: nothing is reached after the page
    // its last edge leads to, so no later edge refuses the file map for it.
    const std::size_t leaf_depth = nodes_.back().depth;
    const image_node& node = kind == 3 && leaf_depth > 0 ? last_internal_at(in.below(leaf_depth))
                                                         : nodes_[in.below(nodes_.size())];
    if (page(node).size > size || node.offset > size - page(node).size) {
      return;  // cut off by an earlier damage
    }
    switch (kind) {
      case 3:
        if (!node.is_leaf) {
          const std::size_t last_edge = len(node) < capacity(node) ? len(node) : capacity(node);
          const std::uint64_t tag = get<std::uint64_t>(edge_slot(node, last_edge)) & 1;
          const std::size_t align = tag != 0 ? types::leaf.page.align : types::internal.page.align;
          put(edge_slot(node, last_edge), ((0 - align * (1 + in.below(4))) | tag) - node.offset);
        }
        break;
      case 4:
        if (!node.is_leaf) {
          // The child's place in the file map, or the edge's own word.
          const std::size_t edge = in.below(capacity(node) + 1);
          const std::uint64_t target = extreme(in, types::leaf.page.align);
          put(edge_slot(node, edge), in.byte() % 2 == 0 ? target - node.offset : target);
        }
        break;
      case 5:
        if (node.is_leaf) {
          resize_leaf(node, in.below(types::leaf_capacity + 2));
        } else {
          put(len_slot(node), static_cast<std::uint16_t>(in.below(types::internal_capacity + 2)));
        }
        break;
      case 6:
        if (node.is_leaf) {
          set_head_flag(node, static_cast<std::uint8_t>(in.below(3)));
        } else {
          put(head_flag_slot(node), in.byte());
        }
        break;
      case 7:
        if constexpr (types::has_inline_keys) {
          put(prefix_len_slot(node),
              static_cast<std::uint16_t>(in.byte() % 2 == 0 ? in.below(20) : in.u64()));
        }
        break;
      case 8: {
        // A node's header, inline keys and hash tags.
        const std::size_t at = node.offset + in.below(page(node).size < 400 ? page(node).size : 400);
        put(at, in.byte());
        break;
      }
      default:
        if (node.is_leaf) {
          // A slot's two halves: what a span keeps as its length and offset.
          const std::size_t first = types::leaf.head_key_offset.value_or(types::leaf.head_offset) / 8 * 8;
          const std::size_t at = node.offset + first + in.below((types::leaf.page.size - first) / 8) * 8;
          put(at, half_extreme(in, size, at));
          put(at + 4, half_extreme(in, size, at));
        }
    }
  }

 private:
  static std::vector<image_node> nodes_of(std::span<const std::byte> valid) {
    const auto word = [&](std::size_t at) {
      std::uint64_t value;
      std::memcpy(&value, valid.data() + at, sizeof value);
      return value;
    };
    std::vector<image_node> found;
    std::vector<std::uint64_t> level;
    if (const std::uint64_t root = word(root_slot); root != 0) {
      level.push_back(root);
    }
    for (std::size_t depth = 0; !level.empty(); ++depth) {
      std::vector<std::uint64_t> children;
      for (const std::uint64_t tagged : level) {
        const image_node node{tagged & ~std::uint64_t{1}, (tagged & 1) != 0, depth};
        found.push_back(node);
        if (!node.is_leaf) {
          std::uint16_t len;
          std::memcpy(&len, valid.data() + len_slot(node), sizeof len);
          for (std::size_t edge = 0; edge <= len; ++edge) {
            children.push_back(node.offset + word(edge_slot(node, edge)));
          }
        }
      }
      level = std::move(children);
    }
    return found;
  }

  std::string corruption_from(reader (*open)(std::span<const std::byte>)) const {
    try {
      (void)open(bytes());
    } catch (const bbtree::file_error& error) {
      if (error.why() != bbtree::file_error::kind::corrupt) {
        throw;
      }
      return error.what();
    }
    return {};
  }

  void rebuffer(std::span<const std::byte> bytes) {
    bbtree::file_buf buffer(bytes.size(), reader::alignment());
    if (!bytes.empty()) {
      std::memcpy(buffer.bytes().data(), bytes.data(), bytes.size());
    }
    buffer_ = std::move(buffer);
  }

  // An offset or a count a check must survive: the ends of the address
  // space, the file map's end, and pages of `align` just short of wrapping.
  std::uint64_t extreme(fuzz_input& in, std::uint64_t align) {
    const std::uint64_t size = bytes().size();
    switch (in.byte() % 12) {
      case 0:
        return ~std::uint64_t{0};
      case 1:
        return ~std::uint64_t{0} - 1;
      case 2:
        return std::uint64_t{1} << 63;
      case 3:
        return (std::uint64_t{1} << 63) | 1;
      case 4:
        return 0 - align * (1 + in.below(8));
      case 5:
        return (0 - align * (1 + in.below(8))) | 1;
      case 6:
        return size;
      case 7:
        return size | 1;
      case 8: {
        const std::uint64_t near_end = size - align * in.below(4);
        return near_end | in.below(2);
      }
      case 9:
        return in.below(4096);
      case 10: {
        const std::uint64_t aligned = in.below(size / align + 2) * align;
        return aligned | in.below(2);
      }
      default:
        return in.u64();
    }
  }

  // Half of a slot at `at`: a length or an offset a span's check must
  // survive.
  static std::uint32_t half_extreme(fuzz_input& in, std::size_t size, std::size_t at) {
    switch (in.byte() % 9) {
      case 0:
        return 0xffffffffu;
      case 1:
        return 0xfffffffeu;
      case 2:
        return 0x80000000u;
      case 3:
        return 0;
      case 4:
        return 1;
      case 5:
        return 8;
      case 6:
        return static_cast<std::uint32_t>(size);
      case 7:
        return static_cast<std::uint32_t>(size - at);
      default:
        return static_cast<std::uint32_t>(in.u64());
    }
  }

  std::vector<image_node> nodes_;
  bbtree::file_buf buffer_;
};

}  // namespace bbtree_fuzz
