// The compile-time description of one map type's tree: its key and value
// types, comparator and strategy, resolved into the node plans and layouts
// every node operation reads (D47, D21). The Rust crate threads `K, V, S`
// through every node type and asks for the plan where it is used; here one
// type, `tree_types`, carries all of it, and node types take it as their one
// parameter.
//
// Also here: `separator`, a separator on its way between nodes, and
// `search_key`, what a search looks for; both depend on whether the map has
// inline keys.
#pragma once

#include <bbtree/detail/latched/latch.hpp>
#include <bbtree/detail/node/keys.hpp>
#include <bbtree/detail/node/layout.hpp>

#include <compare>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace bbtree::detail {

template <class S>
struct strategy_traits<latched<S>> : strategy_traits<S> {
  static constexpr bool latched = true;
};

// A file map's strategy (D58): `S`'s inline keys, nodes read where they lie
// in a file map, each edge the offset of its child from its node's page.
template <class S>
struct in_file {};

template <class S>
struct strategy_traits<in_file<S>> : strategy_traits<S> {
  static constexpr bool in_file = true;
};

template <class K, class V, class Compare, class Strategy>
struct tree_types {
  using key_type = K;
  using mapped_type = V;
  using key_compare = Compare;
  using order = key_order<Compare>;
  using strategy = strategy_traits<Strategy>;
  using inliner = key_inliner<K, Compare, typename strategy::inline_keys>;
  using config = typename strategy::config;
  using annex = typename strategy::annex;
  using layouts = node_layout<K, V, config, annex>;

  static_assert(std::is_nothrow_move_constructible_v<K> && std::is_nothrow_destructible_v<K>,
                "keys move between nodes, which must not throw (D48)");
  static_assert(std::is_nothrow_move_constructible_v<V> && std::is_nothrow_destructible_v<V>,
                "values move between nodes, which must not throw (D48)");

  static constexpr bool latched = strategy::latched;
  static constexpr bool in_file = strategy::in_file;
  static constexpr bool has_inline_keys = inliner::has_inline_keys;
  static_assert(!latched || has_inline_keys, "a shared map's keys need inline keys (D54)");

  // Whether each leaf keeps an annex after its header (D60): only a shared
  // map's leaves do.
  static constexpr bool has_leaf_annex = !std::is_void_v<annex>;
  static_assert(!has_leaf_annex || (latched && !in_file), "only a shared map's leaves keep an annex (D60)");

  static constexpr node_plan leaf_plan = [] {
    node_plan plan = node_plan::of<K, inliner>(true);
    if (plan.hash_tags) {
      plan.hash_tags = layouts::leaf(plan.width, true, latched).hash_tags_offset.has_value();
    }
    return plan;
  }();
  static constexpr node_plan internal_plan = node_plan::of<K, inliner>(false);

  static constexpr leaf_layout leaf = layouts::leaf(leaf_plan.width, leaf_plan.hash_tags, latched);
  static constexpr internal_layout internal = layouts::internal(internal_plan.width, latched);

  // Whether a leaf pairs keys with values in entries (D29): with inline keys,
  // unless values take no room.
  static constexpr bool paired_leaf_entries = leaf.keys.key_stride != slot_size<K>;

  // The root page holds a node of either kind: a page of the larger kind.
  static constexpr bool root_is_leaf_sized = leaf.page.size >= internal.page.size;
  static constexpr page_layout root_page = root_is_leaf_sized ? leaf.page : internal.page;
  static_assert(root_page.align >= leaf.page.align && root_page.align >= internal.page.align,
                "the larger node kind is at least as aligned as the other (D52)");

  // Where a leaf's annex lies, from the start of its page.
  static constexpr std::size_t leaf_annex_offset = layouts::annex_offset(latched);

  static constexpr std::size_t leaf_capacity = leaf.keys.capacity;
  static constexpr std::size_t internal_capacity = internal.keys.capacity;

  // The fewest keys a node below the root holds between removals (D22).
  static constexpr std::size_t min_len(std::size_t capacity) {
    return std::max<std::size_t>(capacity / 8, 4);
  }
  static constexpr std::size_t leaf_min_len = min_len(leaf_capacity);
  static constexpr std::size_t internal_min_len = min_len(internal_capacity);
};

// A separator outside its internal node (D17): on its way up from a split,
// between internal nodes, or into a merged leaf. Without inline keys the
// internal nodes keep their separator keys, and a separator carries its key;
// with inline keys it is the key's window alone, the key staying a leaf head
// (D49).
template <class T, bool CarriesKey = !T::has_inline_keys>
class separator;

template <class T>
class separator<T, true> {
 public:
  using key_type = typename T::key_type;

  explicit separator(windowed_key<key_type>&& key) noexcept : key_(std::move(key)) {}

  bbtree::detail::window window() const noexcept { return key_.window; }
  separator moved_back_to(bbtree::detail::window fill) && noexcept {
    key_.window = key_.window.moved_back_to(fill);
    return std::move(*this);
  }
  windowed_key<key_type> take_key() && noexcept { return std::move(key_); }
  const key_type& key() const noexcept { return key_.key; }

 private:
  windowed_key<key_type> key_;
};

template <class T>
class separator<T, false> {
 public:
  explicit separator(bbtree::detail::window window) noexcept : window_(window) {}

  bbtree::detail::window window() const noexcept { return window_; }
  separator moved_back_to(bbtree::detail::window fill) && noexcept {
    return separator(window_.moved_back_to(fill));
  }

 private:
  bbtree::detail::window window_;
};

// What a search looks for: `key`, of type `Q`, in a map of `T`. Where they
// can, the nodes compare its window at each node's prefix with their inline
// keys (D18): for std byte-string keys when `Q` is a std byte string too, and
// for keys with an `inline_key` specialization when `Q` is the key type.
// Otherwise the search compares full keys.
template <class T, class Q>
class search_key {
  using K = typename T::key_type;
  using inliner = typename T::inliner;
  using order = typename T::order;

 public:
  // Whether the nodes compare this query's windows with their inline keys.
  static constexpr bool compares_by_bytes = inliner::is_byte_string && std_byte_string<Q>;
  static constexpr bool compares_by_trait =
      inliner::kind == inliner_kind::trait && std::is_same_v<std::remove_cvref_t<Q>, K>;
  static constexpr bool compares_inline = compares_by_bytes || compares_by_trait;

  explicit search_key(const Q& key) noexcept : key_(key) {
    if constexpr (compares_by_bytes) {
      bytes_ = search_bytes(byte_string<Q>::bytes(key));
    }
  }

  const Q& key() const noexcept { return key_; }

  // The query's bytes, for a byte-string query.
  std::span<const std::uint8_t> bytes() const noexcept
    requires(compares_by_bytes)
  {
    return bytes_.bytes();
  }

  // The query's window at a node's prefix length, all 8 bytes; the node keeps
  // fewer as it compares. Only for a query that `compares_inline`.
  bbtree::detail::window inline_target(std::size_t prefix_len) const noexcept {
    static_assert(compares_inline);
    if constexpr (compares_by_bytes) {
      return {prefix_len, bytes_.inline_key_at(prefix_len)};
    } else {
      return {prefix_len, inliner::inline_key_at(key_, prefix_len)};
    }
  }

  // How the query orders against `stored`, from the start.
  std::weak_ordering cmp_full(const K& stored) const noexcept {
    if constexpr (compares_by_bytes) {
      return bytes_.cmp_from(inliner::bytes(stored), 0);
    } else {
      return order::cmp(key_, stored);
    }
  }

  // How the query orders against `stored`, a key of a node whose plan keeps
  // `width` bytes of each inline key, given that their windows at
  // `target.prefix_len()` tie in those bytes: byte strings, and keys whose
  // specialization has cmp_from, compare past the bytes known equal (D43).
  std::weak_ordering cmp_tied(inline_width width, bbtree::detail::window target,
                              const K& stored) const noexcept {
    const std::size_t equal_len = target.prefix_len() + static_cast<std::size_t>(width);
    if constexpr (compares_by_bytes) {
      return bytes_.cmp_from(inliner::bytes(stored), equal_len);
    } else if constexpr (compares_by_trait && inliner::trait_has_cmp_from) {
      return inline_key<K>::cmp_from(key_, stored, equal_len);
    } else {
      return order::cmp(key_, stored);
    }
  }

  // Whether the query's bytes equal `stored`'s, all at once (D38).
  bool equals_whole_bytes(const K& stored) const noexcept
    requires(compares_by_bytes)
  {
    const std::span<const std::uint8_t> other = inliner::bytes(stored);
    return bytes_.bytes().size() == other.size() &&
           std::memcmp(bytes_.bytes().data(), other.data(), other.size()) == 0;
  }

  // Whether the query has a hash tag to compare with a leaf's (D36).
  static constexpr bool has_hash_tag =
      compares_by_bytes || (compares_by_trait && inliner::trait_has_hash_tag);

  std::uint8_t hash_tag() const noexcept {
    static_assert(has_hash_tag);
    if constexpr (compares_by_bytes) {
      const std::size_t len = bytes_.bytes().size();
      return len < 255 ? static_cast<std::uint8_t>(len) : std::uint8_t{255};
    } else {
      return inline_key<K>::hash_tag(key_);
    }
  }

  // How the query orders against every key of a tree whose encodings start
  // with `shared` (D50): before or after them all if it differs there, or as
  // a byte string ends inside them; equal if it has them.
  std::strong_ordering cmp_shared_prefix(std::span<const std::uint8_t> shared) const noexcept {
    static_assert(compares_inline);
    if constexpr (compares_by_bytes) {
      return bytes_.cmp_shared(shared);
    } else {
      return inliner::cmp_prefix(key_, shared);
    }
  }

 private:
  const Q& key_;
  search_bytes bytes_;
};

}  // namespace bbtree::detail
