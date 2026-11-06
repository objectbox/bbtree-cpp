// Node pages: the tagged pointer every edge, node stack and root keeps
// (`node_ptr`, D52), the allocation boundary (`fresh_page`), and raw access
// to an initialized node's header and slot arrays: the arrays beside its keys
// (`key_columns`), its keys with them (`key_slots`), and an internal node's
// separators (`separator_slots`).
//
// Node handles and their contracts (the Rust crate's `tree/node/mod.rs`): a
// handle is a page pointer with no access of its own. Code holding one has the
// access of the operation it is part of. Indexes are keys (`index < len`) or
// edges (`index <= len`), as each method says; checked in debug builds. The
// node invariants:
//  1. A node holds a well-formed node of its kind; `len <= capacity`; keys,
//     values, separators and edges 0..len (0..=len for edges) are live.
//  2. Every edge owns a distinct node one level down; all leaves lie at one
//     depth; every pointer's tag tells its node's kind.
//  3. A leaf's head (and head key with inline keys) is live exactly when its
//     header says it has one.
//  4. Inline key i is key i's window at the node's prefix length; slots past
//     `len` hold the greatest inline key, all ones.
//  5. A leaf's live hash tags are its keys' tags.
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/detail/node/inline_key_search.hpp>
#include <bbtree/detail/node/slots.hpp>
#include <bbtree/detail/node/tree_types.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace bbtree::detail {

// Where pages come from and go back to: a map's allocator, which throws
// `alloc_error` when it has none (the caller takes pages before it changes
// anything), or a shared map's pages taken beforehand.
template <class A>
class page_source {
 public:
  explicit page_source(const A& allocator) noexcept : allocator_(allocator) {}

  void* allocate(page_kind kind, page_layout layout) const {
    void* page = page_allocator_traits<A>::allocate(allocator_, kind, layout);
    if (page == nullptr) {
      throw alloc_error();
    }
    return page;
  }

  void* try_allocate(page_kind kind, page_layout layout) const noexcept {
    return page_allocator_traits<A>::allocate(allocator_, kind, layout);
  }

  void deallocate(page_kind kind, void* page, page_layout layout) const noexcept {
    page_allocator_traits<A>::deallocate(allocator_, kind, page, layout);
  }

  const A& allocator() const noexcept { return allocator_; }

 private:
  const A& allocator_;
};

// What a page is for: a leaf, an internal node, or the root (a page of the
// larger kind, D52).
enum class page_role : std::uint8_t { leaf, internal, root };

template <class T>
struct page_role_traits {
  static constexpr page_layout layout(page_role role) {
    switch (role) {
      case page_role::leaf:
        return T::leaf.page;
      case page_role::internal:
        return T::internal.page;
      case page_role::root:
        return T::root_page;
    }
    return T::root_page;
  }
  static constexpr page_kind kind(page_role role) {
    switch (role) {
      case page_role::leaf:
        return page_kind::leaf;
      case page_role::internal:
        return page_kind::internal;
      case page_role::root:
        return T::root_is_leaf_sized ? page_kind::leaf : page_kind::internal;
    }
    return page_kind::leaf;
  }
};

// An allocated page that holds no node, of role `Role`: the only way to make
// a node, and what a freed node turns back into. It owns its page, as a box
// with nothing in it; dropping it without giving it back leaks the page.
template <class T, page_role Role>
class fresh_page {
 public:
  static constexpr page_layout layout = page_role_traits<T>::layout(Role);
  static constexpr page_kind kind = page_role_traits<T>::kind(Role);

  // A page from `pages`: a map's `page_source`, or pages reserved before a
  // change (`reserved_pages`).
  template <class Pages>
  static fresh_page allocate(const Pages& pages) {
    return fresh_page(pages.allocate(kind, layout));
  }

  // Takes ownership of `page`, allocated for this role's layout.
  static fresh_page adopt(void* page) noexcept { return fresh_page(page); }

  template <class Pages>
  void deallocate(const Pages& pages) && noexcept {
    pages.deallocate(kind, page_, layout);
  }

  std::byte* release() && noexcept { return static_cast<std::byte*>(page_); }

 private:
  explicit fresh_page(void* page) noexcept : page_(page) {
    assert(reinterpret_cast<std::uintptr_t>(page) % layout.align == 0 && "a page aligned to its layout");
  }

  void* page_;
};

// A fresh page whose role is known only at run time: what freeing a node of
// either kind gives back.
template <class T>
class any_page {
 public:
  template <page_role Role>
  any_page(fresh_page<T, Role>&& page) noexcept  // NOLINT(google-explicit-constructor)
      : role_(Role), page_(std::move(page).release()) {}

  template <class Pages>
  void deallocate(const Pages& pages) && noexcept {
    pages.deallocate(page_role_traits<T>::kind(role_), page_, page_role_traits<T>::layout(role_));
  }

  page_role role() const noexcept { return role_; }

 private:
  page_role role_;
  std::byte* page_;
};

// The arrays a node keeps beside its keys, one slot per key: its inline keys
// at its plan's width, and in a leaf whose plan keeps them, its keys' hash
// tags (D36). Every key move moves them alike.
template <class T, bool IsLeaf>
class key_columns {
 public:
  static constexpr node_plan plan = IsLeaf ? T::leaf_plan : T::internal_plan;
  static constexpr key_area area = IsLeaf ? T::leaf.keys : T::internal.keys;
  static constexpr std::size_t capacity = area.capacity;
  static constexpr bool has_inline_keys = plan.width.has_value();
  static constexpr bool has_hash_tags = IsLeaf && plan.hash_tags;
  static constexpr inline_width width = plan.width.value_or(inline_width::eight);
  using slot_traits = inline_slot<width>;
  using inline_slot_type = typename slot_traits::type;

  explicit key_columns(std::byte* page) noexcept : page_(page) {}

  // The inline keys, read in place for a search.
  node_inline_keys<width, capacity> inline_keys(std::size_t len) const noexcept
    requires has_inline_keys
  {
    return {page_ + *area.inline_keys_offset, len};
  }

  bbtree::detail::hash_tags<capacity> tags() const noexcept
    requires has_hash_tags
  {
    return bbtree::detail::hash_tags<capacity>(hash_tag_bytes());
  }

  const std::uint8_t* hash_tag_bytes() const noexcept
    requires has_hash_tags
  {
    return reinterpret_cast<const std::uint8_t*>(page_ + *T::leaf.hash_tags_offset);
  }

  inline_word read_inline_key(std::size_t index) const noexcept {
    if constexpr (has_inline_keys) {
      return slot_traits::inline_key(inline_key_slots().load(index));
    } else {
      return 0;
    }
  }

  void write_inline_key(std::size_t index, inline_word inline_key) const noexcept {
    if constexpr (has_inline_keys) {
      inline_slot_type slot = slot_traits::of(inline_key);
      std::memcpy(page_ + *area.inline_keys_offset + index * sizeof slot, &slot, sizeof slot);
    }
  }

  // Fills every slot of a node with no keys yet: inline keys the greatest,
  // hash tags 0.
  void initialize() const noexcept {
    if constexpr (has_inline_keys) {
      inline_key_slots().fill_bytes(capacity, 0xff);
    }
    if constexpr (has_hash_tags) {
      tag_slots().fill_bytes(capacity, 0);
    }
  }

  // Gives the slots a node no longer holds keys in the greatest inline key
  // again; their tags stay stale, masked out by every search.
  void mark_vacant(std::size_t start, std::size_t end) const noexcept {
    for (std::size_t index = start; index < end; ++index) {
      write_inline_key(index, ~inline_word{0});
    }
  }

  void write(std::size_t index, inline_word inline_key, std::uint8_t hash_tag) const noexcept {
    write_inline_key(index, inline_key);
    if constexpr (has_hash_tags) {
      tag_slots().construct(index, std::uint8_t{hash_tag});
    }
  }

  void insert(std::size_t index, std::size_t len, inline_word inline_key,
              std::uint8_t hash_tag) const noexcept {
    if constexpr (has_inline_keys) {
      inline_key_slots().insert(index, len, slot_traits::of(inline_key));
    }
    if constexpr (has_hash_tags) {
      tag_slots().insert(index, len, std::uint8_t{hash_tag});
    }
  }

  void remove(std::size_t index, std::size_t len) const noexcept {
    if constexpr (has_inline_keys) {
      inline_key_slots().remove(index, len);
    }
    if constexpr (has_hash_tags) {
      tag_slots().remove(index, len);
    }
  }

  void move_run_to(std::size_t start, std::size_t count, key_columns dest,
                   std::size_t dest_start) const noexcept {
    if constexpr (has_inline_keys) {
      inline_key_slots().move_run_to(start, count, dest.inline_key_slots(), dest_start);
    }
    if constexpr (has_hash_tags) {
      tag_slots().move_run_to(start, count, dest.tag_slots(), dest_start);
    }
  }

 private:
  slots<inline_slot_type> inline_key_slots() const noexcept {
    return {page_ + *area.inline_keys_offset, sizeof(inline_slot_type)};
  }
  slots<std::uint8_t> tag_slots() const noexcept { return {page_ + *T::leaf.hash_tags_offset, 1}; }

  std::byte* page_;
};

// A node's keys and the columns beside them: every operation moves a key and
// its cells together, so inline key i is always key i's. A key comes out
// with its window at the node's prefix length, and must go in with it.
template <class T, bool IsLeaf>
class key_slots {
  using K = typename T::key_type;
  using columns_type = key_columns<T, IsLeaf>;

 public:
  static constexpr key_area area = columns_type::area;

  key_slots(std::byte* page, std::size_t prefix_len) noexcept
      : keys_(page + area.keys_offset, area.key_stride), columns_(page), prefix_len_(prefix_len) {}

  slots<K> keys() const noexcept { return keys_; }
  columns_type columns() const noexcept { return columns_; }

  bbtree::detail::window window(std::size_t index) const noexcept {
    return {prefix_len_, columns_.read_inline_key(index)};
  }

  windowed_key<K> read(std::size_t index) const noexcept { return {keys_.take(index), window(index)}; }

  void write(std::size_t index, windowed_key<K>&& key) const noexcept {
    check_prefix(key.window);
    const std::uint8_t tag = tag_of(key.key);
    columns_.write(index, key.window.inline_key(), tag);
    keys_.construct(index, std::move(key.key));
  }

  windowed_key<K> replace(std::size_t index, windowed_key<K>&& key) const noexcept {
    windowed_key<K> old = read(index);
    write(index, std::move(key));
    return old;
  }

  void insert(std::size_t index, std::size_t len, windowed_key<K>&& key) const noexcept {
    check_prefix(key.window);
    columns_.insert(index, len, key.window.inline_key(), tag_of(key.key));
    keys_.insert(index, len, std::move(key.key));
  }

  windowed_key<K> remove(std::size_t index, std::size_t len) const noexcept {
    const bbtree::detail::window removed_window = window(index);
    columns_.remove(index, len);
    return {keys_.remove(index, len), removed_window};
  }

  void move_run_to(std::size_t start, std::size_t count, key_slots dest,
                   std::size_t dest_start) const noexcept {
    assert(prefix_len_ == dest.prefix_len_ && "a run moves between nodes of one prefix length");
    keys_.move_run_to(start, count, dest.keys_, dest_start);
    columns_.move_run_to(start, count, dest.columns_, dest_start);
  }

 private:
  static std::uint8_t tag_of(const K& key) noexcept {
    if constexpr (columns_type::has_hash_tags) {
      return T::inliner::hash_tag(key);
    } else {
      (void)key;
      return 0;
    }
  }
  void check_prefix([[maybe_unused]] bbtree::detail::window window) const noexcept {
    assert((!columns_type::has_inline_keys || window.prefix_len() == prefix_len_) &&
           "a key goes into a node at its own prefix length");
  }

  slots<K> keys_;
  columns_type columns_;
  std::size_t prefix_len_;
};

// A tagged pointer to a node of a map of `T`: the lowest bit says whether the
// node is a leaf, which pages aligned to 64 bytes or more leave free (D52).
// The tag is the only record of a node's kind.
template <class T>
class node_ptr {
 public:
  static constexpr std::uintptr_t leaf_tag = 1;

  constexpr node_ptr() noexcept = default;

  static node_ptr leaf_at(std::byte* page) noexcept { return node_ptr(address(page) | leaf_tag); }
  static node_ptr internal_at(std::byte* page) noexcept { return node_ptr(address(page)); }
  static node_ptr from_tagged(std::uintptr_t tagged) noexcept { return node_ptr(tagged); }

  std::uintptr_t tagged() const noexcept { return tagged_; }
  bool is_leaf() const noexcept { return (tagged_ & leaf_tag) != 0; }
  explicit operator bool() const noexcept { return tagged_ != 0; }
  std::byte* page() const noexcept { return reinterpret_cast<std::byte*>(tagged_ & ~leaf_tag); }

  friend bool operator==(node_ptr, node_ptr) = default;

  // Header access.
  std::size_t len() const noexcept { return header()->len; }
  void set_len(std::size_t len) const noexcept { header()->len = static_cast<std::uint16_t>(len); }
  bool has_head() const noexcept { return header()->has_head; }
  void set_has_head(bool has_head) const noexcept { header()->has_head = has_head; }

  std::size_t prefix_len() const noexcept {
    if constexpr (T::has_inline_keys) {
      return inline_header()->prefix_len;
    } else {
      return 0;
    }
  }
  void set_prefix_len(std::size_t prefix_len) const noexcept {
    if constexpr (T::has_inline_keys) {
      assert(prefix_len <= inline_key_node_header::max_prefix_len);
      inline_header()->prefix_len = static_cast<std::uint16_t>(prefix_len);
    }
  }

  // Makes the page at `page` an empty node of the given kind, whatever it
  // held: no keys, no head, every inline-key slot the greatest.
  template <bool IsLeaf>
  static node_ptr initialize(std::byte* page) noexcept {
    const node_header header{0, false};
    if constexpr (T::has_inline_keys) {
      ::new (static_cast<void*>(page)) inline_key_node_header{header, 0};
    } else {
      ::new (static_cast<void*>(page)) node_header(header);
    }
    key_columns<T, IsLeaf>(page).initialize();
    return IsLeaf ? leaf_at(page) : internal_at(page);
  }

  // Moves this node, of the given kind, whole into the page at `dest` as
  // bytes, and returns its pointer there. Keys and values move as their
  // relocation allows: as bytes if they are trivially relocatable, and
  // otherwise one at a time (`relocate_node_to`).
  template <bool IsLeaf>
  node_ptr move_node_to(std::byte* dest) const noexcept;

 private:
  explicit node_ptr(std::uintptr_t tagged) noexcept : tagged_(tagged) {}
  static std::uintptr_t address(std::byte* page) noexcept {
    assert((reinterpret_cast<std::uintptr_t>(page) & leaf_tag) == 0);
    return reinterpret_cast<std::uintptr_t>(page);
  }
  node_header* header() const noexcept { return std::launder(reinterpret_cast<node_header*>(page())); }
  inline_key_node_header* inline_header() const noexcept {
    return std::launder(reinterpret_cast<inline_key_node_header*>(page()));
  }

  std::uintptr_t tagged_ = 0;
};

}  // namespace bbtree::detail
