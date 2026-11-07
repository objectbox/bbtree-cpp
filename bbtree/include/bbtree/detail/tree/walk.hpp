// Visiting every entry of a tree in key order, a leaf at a time: what the
// maps' `for_each` runs, as Rust's iterators run their `fold` (PORTING.md,
// P16).
//
// Every entry but the first leaf's is a leaf's key or a leaf's head (D17), so
// visiting the leaves left to right, each leaf's head first, visits every
// entry in order, and no separator on its own. The walk goes down the edges
// depth-first, with no node stack: a cursor's crossings keep the CPU from
// starting the next map's or leaf's loads before the current ones arrive,
// which costs small maps twice the time (measured on 1M maps of 4 entries).
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <cstddef>

namespace bbtree::detail {

template <class T>
class in_order_walk {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

 public:
  explicit in_order_walk(const tree<T>& whole) noexcept : tree_(whole) {}

  // Runs `visit(key, value)` on every entry in key order. A root leaf, every
  // small map's, is walked here, with no call: a call and its frame per map
  // are instructions that keep the next maps' loads out of the CPU's window.
  template <class Visit>
  void run(Visit& visit) const {
    if (!tree_.has_root()) {
      return;
    }
    const node_ptr<T> root = tree_.root();
    if (root.is_leaf()) {
      leaf_entries(leaf_node<T>(root), nullptr, visit);
    } else {
      subtree(root, nullptr, visit);
    }
  }

 private:
  // The entries of `leaf`, whose head key, if it has one, is `head_key` for
  // keys without inline keys.
  template <class Visit>
  [[gnu::always_inline]] static void leaf_entries(const leaf_node<T>& leaf, const K* head_key, Visit& visit) {
    if (leaf.has_head()) {
      if constexpr (T::has_inline_keys) {
        visit(leaf.head_key(), leaf.head_value());
      } else {
        visit(*head_key, leaf.head_value());
      }
    }
    const std::size_t len = leaf.len();
    for (std::size_t index = 0; index < len; ++index) {
      visit(leaf.key(index), leaf.value(index));
    }
  }

  // The entries under `node`, whose leftmost leaf's head key, if it has one,
  // is `head_key`: the separator left of the subtree, kept in the internal
  // node above for keys without inline keys (D49).
  template <class Visit>
  static void subtree(node_ptr<T> node, const K* head_key, Visit& visit) {
    if (node.is_leaf()) {
      leaf_entries(leaf_node<T>(node), head_key, visit);
      return;
    }
    const internal_node<T> internal(node);
    subtree(internal.edge(0), head_key, visit);
    const std::size_t len = internal.len();
    for (std::size_t index = 0; index < len; ++index) {
      const K* separator = nullptr;
      if constexpr (!T::has_inline_keys) {
        separator = &internal.separator_key(index);
      }
      subtree(internal.edge(index + 1), separator, visit);
    }
  }

  const tree<T>& tree_;
};

}  // namespace bbtree::detail
