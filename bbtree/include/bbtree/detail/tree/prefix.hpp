// Per-node prefix upkeep (D26, 05-fence-key.md): a node with inline keys
// skips the leading bytes its two fences share and stores each key's window
// past them. A child's fences lie within its parent's, so its prefix is never
// shorter than its parent's (D44).
//
// A prefix changes only when a fence does, and only a separator change moves
// a fence. While an operation changes the tree's shape, a prefix that must
// shrink shrinks to the parent's, its windows filled from the parent's
// separator, reading no key. Once the operation is done and its entry
// counted, the nodes whose fences changed are fit to them, reading keys:
// - the halves of each split (`refit_split_halves`);
// - the nodes a removal merged or moved entries between (`remove.hpp`);
// - for a removed separator, the right border left of it first
//   (`refit_left_of_separator`).
// A key that lacks what the tree's keys share shrinks the shared prefix, and
// the border nodes' prefixes with it, reading no key.
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>

namespace bbtree::detail {

template <class T>
template <class KeyRef>
prefixed_key<KeyRef> tree<T>::shrink_shared_prefix_for(KeyRef key) noexcept {
  if constexpr (T::has_inline_keys) {
    const node_ptr<T> top = root();
    if (top.is_leaf() && top.len() == 0) {
      // An empty tree, whose prefix nothing bounds yet, takes the key's.
      shared_ = shared_prefix::of_key<T>(key);
      leaf_node<T>(top).set_empty_prefix_len(shared_.len());
    } else {
      const std::size_t len = shared_.template len_shared_with<T>(key);
      if (len < shared_.len()) {
        shrink_borders_to(len);
      }
    }
  }
  return prefixed_key<KeyRef>(static_cast<KeyRef>(key));
}

// Shrinks what every key shares to its first `len` bytes, and moves every
// border node's prefix back to it, filled from the shared bytes' window.
template <class T>
[[gnu::cold, gnu::noinline]] void tree<T>::shrink_borders_to(std::size_t len) noexcept {
  const window fill = shared_.window_at(len);
  shared_.shrink_to(len);
  forget_rightmost_leaf();
  for (const node_end end : {node_end::first, node_end::last}) {
    node_ptr<T> node = root();
    while (!node.is_leaf()) {
      const internal_node<T> internal(node);
      internal.move_prefix_back_to(fill);
      node = internal.edge(end == node_end::last ? internal.len() : 0);
    }
    leaf_node<T>(node).move_prefix_back_to(fill);
  }
}

// Fits the prefixes of the nodes an insert split to their fences, from the
// leaves up. For each `climbed` from 1 to `levels_climbed`, the node that
// many levels up holds, at `separator_indexes[climbed]`, the separator between
// its split child's halves. The node stack ends at the new entry before and
// after.
template <class T>
void node_stack_mut<T>::refit_split_halves(const std::size_t (&separator_indexes)[max_depth + 1],
                                           std::size_t levels_climbed) noexcept {
  if constexpr (T::has_inline_keys) {
    std::size_t indexes[max_depth + 1];
    indexes[0] = index();
    for (std::size_t climbed = 1; climbed <= levels_climbed; ++climbed) {
      ascend();
      indexes[climbed] = index();
      refit_children_to_fences(separator_indexes[climbed], separator_indexes[climbed] + 1);
    }
    for (std::size_t climbed = levels_climbed; climbed-- > 0;) {
      descend();
      set_index(indexes[climbed]);
    }
  } else {
    (void)separator_indexes;
    (void)levels_climbed;
  }
}

// Fits the prefixes of the bottom internal node's children at the given edges
// to their fences, unless a child's own inline keys show they share no more.
template <class T>
void node_stack_mut<T>::refit_children_to_fences(std::size_t first_edge,
                                                 std::optional<std::size_t> second_edge) noexcept {
  if constexpr (T::has_inline_keys) {
    if (internal().has_leaf_children()) {
      refit_children_of_kind<leaf_node<T>>(first_edge, second_edge);
    } else {
      refit_children_of_kind<internal_node<T>>(first_edge, second_edge);
    }
  } else {
    (void)first_edge;
    (void)second_edge;
  }
}

template <class T>
template <class Child>
void node_stack_mut<T>::refit_children_of_kind(std::size_t first_edge,
                                               std::optional<std::size_t> second_edge) noexcept {
  const std::size_t shared_len = tree_.shared().len();
  const std::pair<const K*, const K*> bounds = fences();
  const internal_node<T> node = internal();
  auto refit = [&](std::size_t edge) {
    const Child child(node.edge(edge));
    if (child.keys_differ_past_prefix()) {
      return;
    }
    const K* child_low = edge > 0 ? &node.separator_key(edge - 1) : bounds.first;
    const K* child_high = edge < node.len() ? &node.separator_key(edge) : bounds.second;
    std::size_t prefix_len = shared_len;
    if (child_low != nullptr && child_high != nullptr) {
      prefix_len = T::inliner::common_prefix_len(*child_low, *child_high);
    }
    child.refit_prefix(std::max(prefix_len, node.prefix_len()));
  };
  refit(first_edge);
  if (second_edge) {
    refit(*second_edge);
  }
}

// Prepares the separator the node stack ends at to give way to its successor,
// the first key of its head's leaf (D17): the successor becomes the upper
// fence of the right border under the edge left of it, whose prefixes shrink
// to what the separator shares with it. Done before anything changes.
template <class T>
void node_stack_mut<T>::refit_left_of_separator() noexcept {
  if constexpr (T::has_inline_keys) {
    const std::size_t at = index();
    const internal_node<T> node = internal();
    const leaf_node<T> head = node.head_leaf(at);
    assert(head.len() > 0 && "the leaf right of a separator has keys");
    const std::size_t prefix_len =
        std::max(T::inliner::common_prefix_len(node.separator_key(at), head.key(0)), node.prefix_len());
    node_ptr<T> left = node.edge(at);
    while (!left.is_leaf()) {
      const internal_node<T> internal(left);
      internal.shrink_prefix_to(prefix_len);
      left = internal.edge(internal.len());
    }
    leaf_node<T>(left).shrink_prefix_to(prefix_len);
  }
}

}  // namespace bbtree::detail
