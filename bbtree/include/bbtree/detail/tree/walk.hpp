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

#include <cassert>
#include <cstddef>
#include <utility>

namespace bbtree::detail {

template <class T>
class in_order_walk {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

 public:
  explicit in_order_walk(const tree<T>& whole) noexcept : tree_(whole) {}

  // Folds every entry in key order: `accumulator = fold(accumulator, key,
  // value)`, from `accumulator`, as Rust's `Iterator::fold`. The
  // accumulator goes through the walk by value, so it can stay in a
  // register: one that a visitor captures by reference stays in memory
  // wherever the reference reaches a call, and every entry then stores and
  // reloads it (PORTING.md, P16).
  //
  // A root leaf, every small map's, is walked here, with no call: a call and
  // its frame per map are instructions that keep the next maps' loads out of
  // the CPU's window.
  template <class A, class Fold>
  A fold(A accumulator, Fold& fold) const {
    if (!tree_.has_root()) {
      return accumulator;
    }
    const node_ptr<T> root = tree_.root();
    if (root.is_leaf()) {
      return root_leaf_entries(leaf_node<T>(root), std::move(accumulator), fold);
    }
    return subtree(root, nullptr, std::move(accumulator), fold);
  }

  // Runs `visit(key, value)` on every entry in key order.
  template <class Visit>
  void run(Visit& visit) const {
    auto visit_entry = [&visit](std::nullptr_t, const K& key, V& value) {
      visit(key, value);
      return nullptr;
    };
    fold(nullptr, visit_entry);
  }

 private:
  // The longest root leaf `root_leaf_entries` folds four entries a round.
  static constexpr std::size_t short_leaf_len = 16;

  // Folds the entries of the root leaf, the leftmost, which has no head. A
  // short one, as small maps' are, four entries a round, each read by loads
  // of its own: over many maps of one size, each such load steps by a page
  // from map to map, a stride the CPU's prefetcher follows, where one load
  // for every entry steps by an entry and then jumps. Rust's fold gets the
  // same from its vectorizer (P16: 1M maps of 4, 6.1 -> 3.5 ns an entry).
  // Other leaves take `leaf_entries`' one loop, which Clang vectorizes from
  // 17 entries; with the rounds beside it, a 1k map's walk took 9% longer.
  template <class A, class Fold>
  [[gnu::always_inline]] static A root_leaf_entries(const leaf_node<T>& leaf, A accumulator, Fold& fold) {
    assert(!leaf.has_head());
    const std::size_t len = leaf.len();
    if (len > short_leaf_len) {
      return leaf_entries(leaf, nullptr, std::move(accumulator), fold);
    }
    std::size_t index = 0;
    for (; index + 4 <= len; index += 4) {
      accumulator = fold(std::move(accumulator), leaf.key(index), leaf.value(index));
      accumulator = fold(std::move(accumulator), leaf.key(index + 1), leaf.value(index + 1));
      accumulator = fold(std::move(accumulator), leaf.key(index + 2), leaf.value(index + 2));
      accumulator = fold(std::move(accumulator), leaf.key(index + 3), leaf.value(index + 3));
    }
    for (; index < len; ++index) {
      accumulator = fold(std::move(accumulator), leaf.key(index), leaf.value(index));
    }
    return accumulator;
  }

  // Folds the entries of `leaf`, whose head key, if it has one, is
  // `head_key` for keys without inline keys.
  template <class A, class Fold>
  [[gnu::always_inline]] static A leaf_entries(const leaf_node<T>& leaf, const K* head_key, A accumulator,
                                               Fold& fold) {
    if (leaf.has_head()) {
      if constexpr (T::has_inline_keys) {
        accumulator = fold(std::move(accumulator), leaf.head_key(), leaf.head_value());
      } else {
        accumulator = fold(std::move(accumulator), *head_key, leaf.head_value());
      }
    }
    const std::size_t len = leaf.len();
    for (std::size_t index = 0; index < len; ++index) {
      accumulator = fold(std::move(accumulator), leaf.key(index), leaf.value(index));
    }
    return accumulator;
  }

  // Folds the entries under `node`, whose leftmost leaf's head key, if it has
  // one, is `head_key`: the separator left of the subtree, kept in the
  // internal node above for keys without inline keys (D49).
  template <class A, class Fold>
  static A subtree(node_ptr<T> node, const K* head_key, A accumulator, Fold& fold) {
    if (node.is_leaf()) {
      return leaf_entries(leaf_node<T>(node), head_key, std::move(accumulator), fold);
    }
    const internal_node<T> internal(node);
    accumulator = subtree(internal.edge(0), head_key, std::move(accumulator), fold);
    const std::size_t len = internal.len();
    for (std::size_t index = 0; index < len; ++index) {
      const K* separator = nullptr;
      if constexpr (!T::has_inline_keys) {
        separator = &internal.separator_key(index);
      }
      accumulator = subtree(internal.edge(index + 1), separator, std::move(accumulator), fold);
    }
    return accumulator;
  }

  const tree<T>& tree_;
};

}  // namespace bbtree::detail
