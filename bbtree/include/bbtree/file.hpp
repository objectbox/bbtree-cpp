// File maps (D58, 15-index-file.md): a map compacted into one byte string,
// its tree's pages one after another, which a reader maps from a file, or
// reads into memory, and uses where it lies. Opening one builds nothing: it
// reads the header, and checks each page once, or trusts them. Its lookups
// run the map's own code, at the map's speed.
//
//   bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map = ...;
//   bbtree::file_buf bytes = bbtree::file_writer<std::uint64_t, std::uint64_t>(map).to_buf();
//   auto read = bbtree::file_map<std::uint64_t, std::uint64_t>::open(bytes.bytes());
//   read.get(21);  // const std::uint64_t*
//
// - The tree's own pages: each node's page is copied, so a file map has the
//   map's layouts, inline keys, hash tags and prefixes. Nothing else of the
//   writer's memory is: a file map depends on its entries alone.
// - Compact: the writer builds a fresh tree of full nodes from the entries
//   in key order (D25).
// - Level order: the root page, each level of internal nodes, the leaves.
// - Edges are offsets from their node, tagged with the child's kind: a file
//   map is position-independent, at any aligned offset of a file.
// - Keys and values are plain bytes here: trivially copyable, without
//   padding. bbtree-mmap adds bytes of any length after each leaf.
// A file map is for one layout: the same key and value types and strategy,
// pointer width and byte order; a reader of another refuses it.
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/detail/tree/cursor.hpp>
#include <bbtree/detail/tree/tree.hpp>
#include <bbtree/unsync/map.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bbtree {

// What a file map may keep in its slots as they are: trivially copyable types
// whose every byte is part of the value, so the file holds no uninitialized
// byte and any bytes read back are some value.
template <class T>
concept plain_bytes = std::is_trivially_copyable_v<T> && (std::has_unique_object_representations_v<T> ||
                                                          std::is_floating_point_v<T> || std::is_empty_v<T>);

// How a file map keeps a key or value type: plain, in its slot as it is (the
// default), or as a slot whose bytes lie after its leaf, which the type's
// library lays out and checks (bbtree-mmap's `var_bytes`). A specialization
// has `has_bytes`, `in_writer` (the type a writer's tree holds instead), a
// `format` number for the fingerprint (0 for a plain type, another for each
// kind of slot with bytes, so that a file map of one is never read as of
// another of its size), and `check(file_bytes, slot)`, which throws
// `file_error` unless the slot at offset `slot` keeps its bytes in bounds.
template <class T>
struct file_storage {
  static constexpr bool has_bytes = false;
  static constexpr std::uint64_t format = 0;
  using in_writer = T;
  static void check(std::span<const std::byte>, std::size_t) noexcept {}
};

// What a writer writes after each leaf: nothing, for plain keys and values.
struct no_leaf_bytes {
  template <class Leaf>
  static std::size_t len(const Leaf&) noexcept {
    return 0;
  }
  template <class Leaf>
  static void write(const Leaf&, std::byte* /*page*/, std::size_t /*page_size*/,
                    std::byte* /*bytes*/) noexcept {}
};

// What a file map's keys and values may be: plain bytes, or a type whose
// file storage keeps bytes after its leaf.
template <class T>
concept file_storable = std::is_trivially_copyable_v<T> && (plain_bytes<T> || file_storage<T>::has_bytes);

// Why a byte string is no file map this reader can use.
class file_error : public std::runtime_error {
 public:
  enum class kind : std::uint8_t {
    not_a_file_map,
    unsupported_version,
    layout_mismatch,
    misaligned,
    corrupt
  };

  file_error(kind why, const char* what) : std::runtime_error(what), kind_(why) {}
  kind why() const noexcept { return kind_; }

 private:
  kind kind_;
};

// A file map's bytes in memory, aligned as a reader needs them.
class file_buf {
 public:
  file_buf() noexcept = default;
  file_buf(std::size_t size, std::size_t align)
      : bytes_(static_cast<std::byte*>(::operator new(size, std::align_val_t{align}))),
        size_(size),
        align_(align) {
    std::memset(bytes_, 0, size);
  }
  file_buf(file_buf&& other) noexcept
      : bytes_(std::exchange(other.bytes_, nullptr)),
        size_(std::exchange(other.size_, 0)),
        align_(other.align_) {}
  file_buf& operator=(file_buf&& other) noexcept {
    if (this != &other) {
      release();
      bytes_ = std::exchange(other.bytes_, nullptr);
      size_ = std::exchange(other.size_, 0);
      align_ = other.align_;
    }
    return *this;
  }
  file_buf(const file_buf&) = delete;
  file_buf& operator=(const file_buf&) = delete;
  ~file_buf() { release(); }

  std::span<std::byte> bytes() noexcept { return {bytes_, size_}; }
  std::span<const std::byte> bytes() const noexcept { return {bytes_, size_}; }

 private:
  void release() noexcept {
    if (bytes_ != nullptr) {
      ::operator delete(bytes_, std::align_val_t{align_});
    }
  }

  std::byte* bytes_ = nullptr;
  std::size_t size_ = 0;
  std::size_t align_ = 1;
};

namespace detail {

// The header at a file map's start: magic, format version, the layout's
// fingerprint, entry count, the root page's tagged offset, the byte length,
// and what every key shares (D50).
struct file_header {
  static constexpr std::size_t size = 128;
  static constexpr std::uint32_t format_version = 2;
  static constexpr std::size_t shared_prefix_offset = 48;
  static constexpr char magic[8] = {'b', 'b', 't', 'r', 'e', 'e', 'I', 'M'};

  std::uint64_t fingerprint = 0;
  std::uint64_t length = 0;
  std::uint64_t root = 0;
  std::uint64_t byte_len = 0;
  shared_prefix shared;

  void write(std::span<std::byte> out) const noexcept {
    std::memcpy(out.data(), magic, 8);
    std::memcpy(out.data() + 8, &format_version, 4);
    const std::uint64_t words[4] = {fingerprint, length, root, byte_len};
    std::memcpy(out.data() + 16, words, sizeof words);
    out[shared_prefix_offset] = static_cast<std::byte>(shared.len());
    std::memcpy(out.data() + shared_prefix_offset + 1, shared.bytes().data(), shared.len());
  }

  static file_header read(std::span<const std::byte> in) {
    if (in.size() < size || std::memcmp(in.data(), magic, 8) != 0) {
      throw file_error(file_error::kind::not_a_file_map, "no file map header");
    }
    std::uint32_t version;
    std::memcpy(&version, in.data() + 8, 4);
    if (version != format_version) {
      throw file_error(file_error::kind::unsupported_version, "another file map format version");
    }
    file_header header;
    std::uint64_t words[4];
    std::memcpy(words, in.data() + 16, sizeof words);
    header.fingerprint = words[0];
    header.length = words[1];
    header.root = words[2];
    header.byte_len = words[3];
    const auto len = static_cast<std::size_t>(in[shared_prefix_offset]);
    if (len > shared_prefix::max_len) {
      throw file_error(file_error::kind::corrupt, "a shared prefix past its maximum");
    }
    header.shared = shared_prefix::of_bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(in.data()) + shared_prefix_offset + 1, len));
    return header;
  }
};

// The layout one map type's file maps have: its fingerprint, alignment, and
// the check that keeps every read of any bytes in bounds.
template <class T>
struct file_format {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

  static constexpr std::size_t alignment = T::root_page.align;
  // The bit of a tagged offset that says its page holds a leaf (D52).
  static constexpr std::uint64_t leaf_tag = node_ptr<T>::leaf_tag;

  // A hash of everything the layout depends on, and of what the slots hold
  // (`file_storage::format`): a reader of another layout refuses the file
  // map.
  static constexpr std::uint64_t fingerprint() noexcept {
    const std::uint64_t fields[] = {
        64,
        std::endian::native == std::endian::little,
        sizeof(K),
        alignof(K),
        slot_size<V>,
        alignof(V),
        file_storage<K>::format,
        file_storage<V>::format,
        static_cast<std::uint64_t>(T::inliner::kind),
        T::leaf_plan.width ? static_cast<std::uint64_t>(*T::leaf_plan.width) : 0,
        T::leaf_plan.hash_tags,
        T::internal_plan.width ? static_cast<std::uint64_t>(*T::internal_plan.width) : 0,
        T::leaf.page.size,
        T::leaf.page.align,
        T::leaf.keys.capacity,
        T::leaf.keys.inline_keys_offset.value_or(~0ull),
        T::leaf.keys.keys_offset,
        T::leaf.keys.key_stride,
        T::leaf.head_offset,
        T::leaf.value_stride,
        T::leaf.hash_tags_offset.value_or(~0ull),
        T::leaf.head_key_offset.value_or(~0ull),
        T::internal.page.size,
        T::internal.page.align,
        T::internal.keys.capacity,
        T::internal.keys.inline_keys_offset.value_or(~0ull),
        T::internal.keys.keys_offset,
        T::internal.keys.key_stride,
        T::internal.edges_offset,
        T::internal.edge_stride};
    std::uint64_t hash = 0xcbf29ce484222325;
    for (const std::uint64_t field : fields) {
      for (int byte = 0; byte < 8; ++byte) {
        hash = (hash ^ ((field >> (8 * byte)) & 0xff)) * 0x100000001b3;
      }
    }
    return hash;
  }

  // Checks the slots of a leaf at `offset` whose bytes lie elsewhere.
  static void check_slots(std::span<const std::byte> bytes, std::size_t offset, std::size_t len,
                          bool has_head) {
    if constexpr (file_storage<K>::has_bytes) {
      for (std::size_t index = 0; index < len; ++index) {
        file_storage<K>::check(bytes, offset + T::leaf.keys.keys_offset + index * T::leaf.keys.key_stride);
      }
      if (has_head) {
        file_storage<K>::check(bytes, offset + *T::leaf.head_key_offset);
      }
    }
    if constexpr (file_storage<V>::has_bytes) {
      for (std::size_t index = has_head ? 0 : 1; index <= len; ++index) {
        file_storage<V>::check(bytes, offset + T::leaf.head_offset + index * T::leaf.value_stride);
      }
    }
  }

  // Whether a page of layout `page` lies whole in `bytes` at `offset`,
  // aligned. The offset is a word of the file map, which may hold anything:
  // `offset + page.size` could wrap, so the sum is never taken.
  static bool holds_page(std::span<const std::byte> bytes, std::uint64_t offset, page_layout page) noexcept {
    return offset % page.align == 0 && offset <= bytes.size() && page.size <= bytes.size() - offset;
  }

  // The header of `bytes`, which a file map of this layout starts with,
  // checked: its magic, version and fingerprint, that `bytes` is as long as
  // it says and aligned, and that its root page lies in it, past the header.
  static file_header read_header(std::span<const std::byte> bytes) {
    const file_header header = file_header::read(bytes);
    if (header.fingerprint != fingerprint()) {
      throw file_error(file_error::kind::layout_mismatch, "a file map of another layout");
    }
    if (header.byte_len > bytes.size()) {
      throw file_error(file_error::kind::corrupt, "a file map longer than its bytes");
    }
    if (reinterpret_cast<std::uintptr_t>(bytes.data()) % alignment != 0) {
      throw file_error(file_error::kind::misaligned, "file map bytes not aligned to its pages");
    }
    if (header.root != 0) {
      const std::uint64_t offset = header.root & ~leaf_tag;
      if (offset < file_header::size || !holds_page(bytes.first(header.byte_len), offset, T::root_page)) {
        throw file_error(file_error::kind::corrupt, "a root page outside the file map");
      }
    }
    return header;
  }

  // Checks every page reachable from the root for what keeps reads in
  // bounds: in the file map, aligned, headers in range, edges leading
  // forward to children of one kind per level, a key in every leaf below the
  // root, a head in every leaf but the first, and the entry count.
  static void check(std::span<const std::byte> bytes, const file_header& header) {
    if (header.root == 0) {
      if (header.length != 0) {
        throw file_error(file_error::kind::corrupt, "entries without a tree");
      }
      return;
    }
    std::vector<std::uint64_t> level{header.root};
    std::uint64_t entries = 0;
    std::uint64_t last_reached = header.root & ~leaf_tag;
    for (std::size_t depth = 0; depth <= max_depth; ++depth) {
      const bool leaves = (level.front() & leaf_tag) != 0;
      const page_layout page = depth == 0 ? T::root_page : leaves ? T::leaf.page : T::internal.page;
      const std::size_t capacity = leaves ? T::leaf_capacity : T::internal_capacity;
      std::vector<std::uint64_t> children;
      // A leaf's head is the entry of the separator left of it (D17): every
      // leaf has one but the first.
      std::uint8_t head_of_leaf = 0;
      for (const std::uint64_t tagged : level) {
        const std::uint64_t offset = tagged & ~leaf_tag;
        if (!holds_page(bytes, offset, page)) {
          throw file_error(file_error::kind::corrupt, "a page outside the file map, or misaligned");
        }
        const std::byte* node = bytes.data() + offset;
        std::uint16_t len;
        std::memcpy(&len, node, sizeof len);
        const auto has_head = static_cast<std::uint8_t>(node[offsetof(node_header, has_head)]);
        if (len > capacity || has_head > 1) {
          throw file_error(file_error::kind::corrupt, "a node header out of range");
        }
        if (leaves) {
          // A walk steps from a separator to the first key of the leaf after
          // it, and back to the last key of the leaf before it
          // (`cursor.hpp`): a leaf below the root holds a key, as every leaf
          // a writer makes does.
          if (depth > 0 && len == 0) {
            throw file_error(file_error::kind::corrupt, "a leaf below the root without keys");
          }
          // A reader takes a separator's key and value from the head of the
          // leaf right of it, whatever that leaf's flag says: the flag must
          // say so, for the head's slots to be checked below.
          if (has_head != head_of_leaf) {
            throw file_error(file_error::kind::corrupt, "a head in the first leaf, or none in a later one");
          }
          head_of_leaf = 1;
          entries += len + has_head;
          check_slots(bytes, offset, len, has_head == 1);
          continue;
        }
        for (std::size_t edge = 0; edge <= len; ++edge) {
          std::uint64_t relative;
          std::memcpy(&relative, node + T::internal.edges_offset + edge * T::internal.edge_stride,
                      sizeof relative);
          const std::uint64_t child = offset + relative;
          if (child < offset || (child & ~leaf_tag) <= last_reached) {
            throw file_error(file_error::kind::corrupt, "an edge that does not lead forward");
          }
          if (!children.empty() && ((children.front() ^ child) & leaf_tag) != 0) {
            throw file_error(file_error::kind::corrupt, "children of both kinds at one depth");
          }
          last_reached = child & ~leaf_tag;
          children.push_back(child);
        }
      }
      if (leaves) {
        if (entries != header.length) {
          throw file_error(file_error::kind::corrupt, "leaves holding another number of entries");
        }
        return;
      }
      level = std::move(children);
    }
    throw file_error(file_error::kind::corrupt, "a tree deeper than a node stack");
  }
};

}  // namespace detail

// Compacts a map, or entries in ascending order, into a file map. For keys or
// values whose bytes lie after their leaf, the writer's tree holds their
// `in_writer` types, and `LeafBytes` lays the bytes out after each leaf.
template <file_storable K, file_storable V, class Compare = std::less<K>, class Strategy = auto_inline_keys,
          class LeafBytes = no_leaf_bytes>
class file_writer {
  using KW = typename file_storage<K>::in_writer;
  using VW = typename file_storage<V>::in_writer;
  using writer_compare = std::conditional_t<std::is_same_v<Compare, std::less<K>>, std::less<KW>, Compare>;
  using types = detail::tree_types<KW, VW, writer_compare, Strategy>;
  using file_types = detail::tree_types<K, V, Compare, detail::in_file<Strategy>>;
  using format = detail::file_format<file_types>;
  static_assert(types::leaf == file_types::leaf && types::internal == file_types::internal &&
                    types::leaf_plan.width == file_types::leaf_plan.width &&
                    types::leaf_plan.hash_tags == file_types::leaf_plan.hash_tags &&
                    types::internal_plan.width == file_types::internal_plan.width,
                "a writer's pages and its file map's are of one layout");

 public:
  // The entries of a map, in key order.
  template <class A>
    requires(std::is_same_v<K, KW> && std::is_same_v<V, VW>)
  explicit file_writer(const unsync::bbtree_map<K, V, Compare, A, Strategy>& map) {
    detail::tree_builder<types, global_allocator> builder(pages());
    for (const auto& [key, value] : map) {
      builder.push(K(key), V(value));
    }
    adopt(std::move(builder).finish());
  }

  // Entries in ascending key order, of the writer's types; throws
  // std::invalid_argument otherwise.
  template <std::input_iterator It>
  static file_writer from_ascending(It first, It last) {
    file_writer writer;
    detail::tree_builder<types, global_allocator> builder(writer.pages());
    std::optional<KW> previous;
    for (; first != last; ++first) {
      const auto& [key, value] = *first;
      if (previous && !types::order::less(*previous, key)) {
        throw std::invalid_argument("file_writer: entries not in ascending key order");
      }
      previous = key;
      builder.push(KW(key), VW(value));
    }
    writer.adopt(std::move(builder).finish());
    return writer;
  }

  file_writer(file_writer&& other) noexcept
      : tree_(std::move(other.tree_)), length_(std::exchange(other.length_, 0)) {}
  file_writer(const file_writer&) = delete;
  file_writer& operator=(const file_writer&) = delete;
  ~file_writer() { tree_.destroy(pages()); }

  static constexpr std::size_t alignment() noexcept { return format::alignment; }

  // How many bytes the file map takes.
  std::size_t byte_len() const noexcept { return layout_pages().second; }

  // Writes the file map into `out`, at least `byte_len()` bytes, which must
  // start at an address aligned to `alignment()` if it is to be read there.
  void write_to(std::span<std::byte> out) const {
    const auto [offsets, end] = layout_pages();
    if (out.size() < end) {
      throw std::invalid_argument("file_writer: buffer too small");
    }
    std::memset(out.data(), 0, end);
    detail::file_header header;
    header.fingerprint = format::fingerprint();
    header.length = length_;
    header.shared = tree_.has_root() ? tree_.shared() : detail::shared_prefix();
    header.byte_len = end;
    if (tree_.has_root()) {
      header.root = offsets.front().offset | (tree_.root().is_leaf() ? 1 : 0);
      std::unordered_map<std::uintptr_t, std::uint64_t> offset_of;
      for (const placed& page : offsets) {
        offset_of.emplace(page.node.tagged(), page.offset);
      }
      for (const placed& page : offsets) {
        copy_page(out, page, offset_of);
      }
    }
    header.write(out);
  }

  file_buf to_buf() const {
    file_buf buf(byte_len(), alignment());
    write_to(buf.bytes());
    return buf;
  }

 private:
  file_writer() = default;

  detail::page_source<global_allocator> pages() const noexcept {
    return detail::page_source<global_allocator>(allocator_);
  }

  void adopt(std::pair<detail::tree<types>, std::size_t>&& built) noexcept {
    tree_ = std::move(built.first);
    length_ = built.second;
  }

  // A page's place in the file map.
  struct placed {
    detail::node_ptr<types> node;
    std::uint64_t offset;
    std::size_t size;
  };

  // Every page in level order with its offset, and the file map's length.
  std::pair<std::vector<placed>, std::uint64_t> layout_pages() const {
    std::vector<placed> order;
    std::uint64_t at =
        (detail::file_header::size + format::alignment - 1) / format::alignment * format::alignment;
    if (!tree_.has_root()) {
      return {std::move(order), detail::file_header::size};
    }
    std::vector<detail::node_ptr<types>> level{tree_.root()};
    bool root = true;
    while (!level.empty()) {
      std::vector<detail::node_ptr<types>> next;
      for (const auto node : level) {
        const page_layout page = root             ? types::root_page
                                 : node.is_leaf() ? types::leaf.page
                                                  : types::internal.page;
        at = (at + page.align - 1) / page.align * page.align;
        order.push_back({node, at, page.size});
        at += page.size;
        if (node.is_leaf()) {
          at += LeafBytes::len(detail::leaf_node<types>(node));
        }
        if (!node.is_leaf()) {
          const detail::internal_node<types> internal(node);
          for (std::size_t edge = 0; edge <= internal.len(); ++edge) {
            next.push_back(internal.edge(edge));
          }
        }
      }
      level = std::move(next);
      root = false;
    }
    return {std::move(order), at};
  }

  // Copies a page's bytes as the file map keeps them: a leaf's slots whose
  // bytes lie after it as `LeafBytes` writes them, an internal node's edges
  // as offsets from it. What a page holds beside its node's entries is the
  // writer's memory, addresses among it, and none of them reaches the file
  // map: the same entries make the same bytes.
  static void copy_page(std::span<std::byte> out, const placed& page,
                        const std::unordered_map<std::uintptr_t, std::uint64_t>& offset_of) {
    std::byte* dest = out.data() + page.offset;
    const std::size_t copied = page.node.is_leaf() ? types::leaf.page.size : types::internal.page.size;
    std::memcpy(dest, page.node.page(), copied);
    // The header's padding byte, which no write initializes.
    dest[offsetof(detail::node_header, has_head) + 1] = std::byte{0};
    if (page.node.is_leaf()) {
      zero_slots_with_bytes(dest);
      LeafBytes::write(detail::leaf_node<types>(page.node), dest, page.size, dest + page.size);
      return;
    }
    const detail::internal_node<types> internal(page.node);
    // Past the node's keys lie its edges, the writer's pointers, and what
    // the page held before: a root page raised from a leaf keeps the leaf's
    // bytes there (`root_page::raise`). Zeros, but for the edges it has.
    const std::size_t keys_end =
        types::internal.keys.keys_offset + internal.len() * types::internal.keys.key_stride;
    std::memset(dest + keys_end, 0, copied - keys_end);
    for (std::size_t edge = 0; edge <= internal.len(); ++edge) {
      const auto child = internal.edge(edge);
      const std::uint64_t child_offset = offset_of.at(child.tagged());
      const std::uint64_t relative = child_offset - page.offset + (child.is_leaf() ? 1 : 0);
      std::memcpy(dest + types::internal.edges_offset + edge * types::internal.edge_stride, &relative,
                  sizeof relative);
    }
  }

  // Zeroes every slot of `leaf_page`, a leaf's copy, whose bytes lie after
  // the leaf: each holds a pointer of the writer's, or held one. `LeafBytes`
  // then writes those of the leaf's entries; the slots past them, and a
  // missing head's, stay zeros.
  static void zero_slots_with_bytes(std::byte* leaf_page) noexcept {
    constexpr auto layout = types::leaf;
    if constexpr (file_storage<K>::has_bytes) {
      for (std::size_t index = 0; index < types::leaf_capacity; ++index) {
        std::memset(leaf_page + layout.keys.keys_offset + index * layout.keys.key_stride, 0, sizeof(KW));
      }
      if (layout.head_key_offset) {
        std::memset(leaf_page + *layout.head_key_offset, 0, sizeof(KW));
      }
    }
    if constexpr (file_storage<V>::has_bytes) {
      for (std::size_t index = 0; index <= types::leaf_capacity; ++index) {
        std::memset(leaf_page + layout.head_offset + index * layout.value_stride, 0, sizeof(VW));
      }
    }
  }

  global_allocator allocator_;
  detail::tree<types> tree_;
  std::size_t length_ = 0;
};

// A file map read where it lies: lookups, bounds and iteration, by shared
// reference, at the map's speed.
template <file_storable K, file_storable V, class Compare = std::less<K>, class Strategy = auto_inline_keys>
class file_map {
  using types = detail::tree_types<K, V, Compare, detail::in_file<Strategy>>;
  using format = detail::file_format<types>;
  using cursor_type = detail::cursor<types>;

 public:
  // Opens `bytes`, checking every page for what keeps reads in bounds, so
  // that any bytes give answers, wrong or not, never undefined behavior.
  static file_map open(std::span<const std::byte> bytes) {
    file_map map = open_unchecked(bytes);
    format::check(bytes, map.header_);
    return map;
  }

  // Opens `bytes`, checking the header alone, its root page's place
  // included: for a trusted file map.
  static file_map open_unchecked(std::span<const std::byte> bytes) {
    return file_map(bytes, format::read_header(bytes));
  }

  file_map(file_map&& other) noexcept
      : bytes_(other.bytes_), header_(other.header_), tree_(std::move(other.tree_)) {}
  file_map(const file_map&) = delete;
  file_map& operator=(const file_map&) = delete;
  ~file_map() { (void)tree_.release_root(); }

  static constexpr std::size_t alignment() noexcept { return format::alignment; }
  std::size_t size() const noexcept { return header_.length; }
  bool empty() const noexcept { return header_.length == 0; }

  template <class Q = K>
  const V* get(const Q& key) const noexcept {
    const auto found = tree_.find(key);
    return found ? found->value() : nullptr;
  }
  template <class Q = K>
  bool contains(const Q& key) const noexcept {
    return tree_.find(key).has_value();
  }

  // Iterates entries in key order: `entry_ref<K, const V>`s.
  class const_iterator {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = std::pair<const K, V>;
    using difference_type = std::ptrdiff_t;
    using reference = entry_ref<K, const V>;

    const_iterator() noexcept = default;
    reference operator*() const noexcept { return {*cursor_.key(), *cursor_.value()}; }
    const_iterator& operator++() noexcept {
      cursor_.next();
      return *this;
    }
    const_iterator& operator--() noexcept {
      cursor_.prev();
      return *this;
    }
    friend bool operator==(const const_iterator& a, const const_iterator& b) noexcept {
      return a.cursor_ == b.cursor_;
    }

   private:
    friend class file_map;
    explicit const_iterator(cursor_type cursor) noexcept : cursor_(std::move(cursor)) {}
    cursor_type cursor_;
  };

  const_iterator begin() const noexcept { return const_iterator(cursor_type::first(&tree_)); }
  const_iterator end() const noexcept { return const_iterator(cursor_type(&tree_)); }
  const_iterator lower_bound(const K& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::before));
  }
  const_iterator upper_bound(const K& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::after));
  }

 private:
  file_map(std::span<const std::byte> bytes, const detail::file_header& header) noexcept
      : bytes_(bytes), header_(header) {
    if (header.root != 0) {
      // The pages are read only; the node code takes them as mutable
      // handles, but a reader never writes.
      auto* base = const_cast<std::byte*>(bytes.data());
      const auto root =
          detail::node_ptr<types>::from_tagged(reinterpret_cast<std::uintptr_t>(base) + header.root);
      tree_ = detail::tree<types>::of_root(detail::root_page<types>(root), header.shared, {});
    }
  }

  std::span<const std::byte> bytes_;
  detail::file_header header_;
  detail::tree<types> tree_;
};

}  // namespace bbtree
