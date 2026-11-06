// What both node kinds share: a node handle's identity, its insertion and
// split geometry, its search results, and what code working at either level
// needs of a node (the Rust crate's `Child` trait): its minimum, whether two
// siblings fit merged, and its prefix moves and refits (D22, D26).
#pragma once

#include <bbtree/detail/node/page.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>

namespace bbtree::detail {

// Where a search ended: key `index` if `found`, otherwise the edge where the
// key belongs.
struct search_result {
  std::size_t index;
  bool found;
};

// The half of a split that takes the new entry, and its index there.
struct insertion_side {
  bool right;
  std::size_t index;

  // Where a full node of `capacity` entries splits for an insertion at edge
  // `insertion_index`, and where the new entry goes. An insertion at either
  // end leaves the arriving side min(2 × minimum, capacity / 2) entries; one
  // in the middle splits evenly (D32). Returns the separator's index.
  static constexpr std::pair<std::size_t, insertion_side> of_split(std::size_t capacity,
                                                                   std::size_t insertion_index,
                                                                   std::size_t minimum) noexcept {
    const std::size_t arriving_len = std::min(2 * minimum, capacity / 2);
    std::size_t separator_index = capacity / 2;
    if (insertion_index == 0) {
      separator_index = arriving_len - 1;
    } else if (insertion_index == capacity) {
      separator_index = capacity - arriving_len;
    }
    if (insertion_index <= separator_index) {
      return {separator_index, {false, insertion_index}};
    }
    return {separator_index, {true, insertion_index - separator_index - 1}};
  }
};

// A full node split in two: the node keeps its lower entries, `separator`
// goes up, `right` holds the upper ones on a fresh page.
template <class T, class Kind>
struct split_result {
  separator<T> middle;
  Kind right;
  insertion_side inserted;
};

// The binary searches over a node's keys (D19), on any list of keys with
// `size()` and `get(index)`: a node's own, or an internal node's head keys.
struct key_list_search {
  // Branchless: every search runs log2(len) steps, each a conditional move,
  // then one comparison classifies the last key not greater than the query.
  template <class T, class List, class Q>
  static search_result branchless(const List& keys, const Q& key) noexcept {
    using order = typename T::order;
    std::size_t size = keys.size();
    if (size == 0) {
      return {0, false};
    }
    std::size_t base = 0;
    while (size > 1) {
      const std::size_t half = size / 2;
      const std::size_t mid = base + half;
      const bool probe_is_greater = order::less(key, keys.get(mid));
      base = probe_is_greater ? base : mid;
      size -= half;
    }
    const auto ordering = order::cmp(keys.get(base), key);
    if (ordering == 0) {
      return {base, true};
    }
    return {ordering < 0 ? base + 1 : base, false};
  }

  // Early exit: stops at an equal key. `cmp_key(probe)` orders the query
  // against `probe`.
  template <class List, class Cmp>
  static search_result early_exit(const List& keys, Cmp cmp_key) noexcept {
    std::size_t low = 0;
    std::size_t high = keys.size();
    while (low < high) {
      const std::size_t mid = low + (high - low) / 2;
      const auto ordering = cmp_key(keys.get(mid));
      if (ordering < 0) {
        high = mid;
      } else if (ordering > 0) {
        low = mid + 1;
      } else {
        return {mid, true};
      }
    }
    return {low, false};
  }
};

// Keys `start..start + size` of a node's slots, as a search reads them.
template <class K>
class node_keys {
 public:
  node_keys(slots<K> keys, std::size_t start, std::size_t size) noexcept
      : keys_(keys), start_(start), size_(size) {}

  std::size_t size() const noexcept { return size_; }
  const K& get(std::size_t index) const noexcept { return *keys_.slot(start_ + index); }

 private:
  slots<K> keys_;
  std::size_t start_;
  std::size_t size_;
};

// The node operations both kinds share, on `Node`, a leaf or internal node
// handle with `len()`, `prefix_len()`, `node()`, `key(index)` and
// `allowed_prefix_len(prefix_len)`.
template <class T, class Node, bool IsLeaf>
class child_node {
 public:
  using columns_type = key_columns<T, IsLeaf>;
  static constexpr node_plan plan = columns_type::plan;
  static constexpr std::size_t capacity = columns_type::capacity;
  // The fewest keys a node below the root holds between removals (D22).
  static constexpr std::size_t min_len = T::min_len(capacity);

  static constexpr bool fit_merged(std::size_t left_len, std::size_t right_len) noexcept {
    return left_len + 1 + right_len <= capacity;
  }

  bool is_underfull() const noexcept { return self().len() < min_len; }
  bool keeps_its_minimum() const noexcept { return self().len() > min_len; }

  // Moves the node's prefix length back to `fill`'s, and every inline key
  // with it, reading no key. Does nothing if the prefix is no longer.
  void move_prefix_back_to(window fill) const noexcept {
    if constexpr (columns_type::has_inline_keys) {
      const std::size_t prefix_len = self().prefix_len();
      if (prefix_len <= fill.prefix_len()) {
        return;
      }
      const columns_type columns(self().node().page());
      const std::size_t len = self().len();
      for (std::size_t index = 0; index < len; ++index) {
        const window moved = window(prefix_len, columns.read_inline_key(index)).moved_back_to(fill);
        columns.write_inline_key(index, moved.inline_key());
      }
      self().node().set_prefix_len(fill.prefix_len());
    }
  }

  // Sets the prefix length of a node with no keys.
  void set_empty_prefix_len(std::size_t prefix_len) const noexcept {
    assert(self().len() == 0 && "a node with keys keeps their prefix length");
    self().node().set_prefix_len(prefix_len);
  }

  // Sets the node's prefix to `prefix_len`, as far as its kind lets it grow,
  // and every inline key to its key's window there, read from the key.
  void refit_prefix(std::size_t prefix_len) const noexcept {
    if constexpr (columns_type::has_inline_keys) {
      prefix_len = self().allowed_prefix_len(prefix_len);
      if (prefix_len == self().prefix_len()) {
        return;
      }
      const columns_type columns(self().node().page());
      const std::size_t len = self().len();
      for (std::size_t index = 0; index < len; ++index) {
        columns.write_inline_key(index, T::inliner::inline_key_at(self().key(index), prefix_len));
      }
      self().node().set_prefix_len(prefix_len);
    } else {
      (void)prefix_len;
    }
  }

  void shrink_prefix_to(std::size_t prefix_len) const noexcept {
    if (self().prefix_len() > prefix_len) {
      refit_prefix(prefix_len);
    }
  }

  // Whether the node's first and last keys differ in the first byte past its
  // prefix: then its fences share no more, and a refit can skip them.
  bool keys_differ_past_prefix() const noexcept {
    if constexpr (columns_type::has_inline_keys) {
      const std::size_t len = self().len();
      if (len == 0) {
        return false;
      }
      const columns_type columns(self().node().page());
      const inline_word differing = columns.read_inline_key(0) ^ columns.read_inline_key(len - 1);
      return std::countl_zero(differing) < 8;
    } else {
      return false;
    }
  }

 private:
  const Node& self() const noexcept { return static_cast<const Node&>(*this); }
};

}  // namespace bbtree::detail
