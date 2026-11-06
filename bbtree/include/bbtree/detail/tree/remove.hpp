// Removing from a tree (D22). The entry comes out where a node stack to it
// ends: out of a leaf, or out of an internal node as a separator. Going back
// up, each node fixes the child it leads to if the removal left that child
// underfull: it merges the child with a sibling, or moves one entry over. A
// merge takes a separator out of the node itself, for its parent to check in
// turn. A root left without separators lowers the tree.
//
// Removing a separator takes its head out: the first key of the head's leaf,
// its successor, takes its place, and the successor's value becomes the new
// head (D17); with inline keys the successor's key becomes the head's key and
// only its window goes up (D49).
//
// The fixing reads no key; once the entry is out and counted, the nodes whose
// fences changed refit their prefixes (`prefix.hpp`).
#pragma once

#include <bbtree/detail/node/siblings.hpp>
#include <bbtree/detail/tree/tree.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

namespace bbtree::detail {

template <class T>
template <class Q, class Pages>
std::optional<std::pair<typename T::key_type, typename T::mapped_type>> tree<T>::remove(
    const Q& query, const Pages& pages, std::size_t& length) noexcept {
  if (!has_root()) {
    return std::nullopt;
  }
  node_stack_mut<T> stack(*this);
  if (!stack.search(search_key<T, Q>(query))) {
    return std::nullopt;
  }
  return stack.remove(pages, length);
}

template <class T>
template <class Pages>
std::optional<std::pair<typename T::key_type, typename T::mapped_type>> tree<T>::pop_first(
    const Pages& pages, std::size_t& length) noexcept {
  if (!has_root()) {
    return std::nullopt;
  }
  node_stack_mut<T> stack(*this);
  stack.start_at_root();
  if (!stack.stack().descend_to_first_key()) {
    return std::nullopt;
  }
  return stack.remove(pages, length);
}

template <class T>
template <class Pages>
std::optional<std::pair<typename T::key_type, typename T::mapped_type>> tree<T>::pop_last(
    const Pages& pages, std::size_t& length) noexcept {
  if (!has_root()) {
    return std::nullopt;
  }
  node_stack_mut<T> stack(*this);
  stack.start_at_root();
  if (!stack.stack().descend_to_last_key()) {
    return std::nullopt;
  }
  return stack.remove(pages, length);
}

// How fixing an underfull child went.
enum class child_fix : std::uint8_t {
  nothing,
  merged_into_left,  // the child, right of its sibling, merged into it
  merged_right_in,   // the right sibling merged into the child
  took_from_left,
  took_from_right,
};

// What a removal changed, by depth: the edge taken at each node on the way
// down to the entry's next slot, the sibling that gave the child at that
// edge an entry, the leaf's depth, and the topmost node that fixed a child.
struct changed_fences {
  std::size_t indexes[max_depth + 1];
  std::optional<std::size_t> donors[max_depth + 1];
  std::size_t leaf_depth;
  std::optional<std::size_t> top_depth;

  changed_fences(std::size_t leaf, std::size_t next_slot, std::optional<std::size_t> top) noexcept
      : leaf_depth(leaf), top_depth(top) {
    indexes[leaf] = next_slot;
  }

  bool reached_root() const noexcept { return top_depth == std::size_t{0}; }

  void lower() noexcept {
    std::copy(indexes + 1, indexes + max_depth + 1, indexes);
    std::copy(donors + 1, donors + max_depth + 1, donors);
    --leaf_depth;
    top_depth = leaf_depth > 0 ? std::optional<std::size_t>(0) : std::nullopt;
  }

  void record(std::size_t depth, child_fix fix, std::size_t left_len) noexcept {
    const std::size_t child = indexes[depth];
    switch (fix) {
      case child_fix::nothing:
        return;
      case child_fix::merged_into_left:
        indexes[depth] = child - 1;
        indexes[depth + 1] += left_len + 1;
        break;
      case child_fix::merged_right_in:
        break;
      case child_fix::took_from_left:
        indexes[depth + 1] += 1;
        donors[depth] = child - 1;
        break;
      case child_fix::took_from_right:
        donors[depth] = child + 1;
        break;
    }
    top_depth = top_depth ? std::min(*top_depth, depth) : depth;
  }
};

// Fixes the child at edge `index` of `parent` if it is underfull: merges it
// with a sibling, the left one if there is one, when both fit with their
// separator; otherwise moves one entry over.
template <class T, class Child>
struct underfull_child {
  template <class Pages>
  static child_fix fix(internal_node<T> parent, std::size_t index, const Pages& pages,
                       std::size_t& left_len) noexcept {
    if (!Child(parent.edge(index)).is_underfull()) {
      return child_fix::nothing;
    }
    const std::size_t sibling = index > 0 ? index - 1 : index + 1;
    assert(sibling <= parent.len() && "an internal node a removal went through has a separator");
    const bool child_is_right = sibling < index;
    const siblings<T, Child> pair(parent, std::min(index, sibling));
    left_len = pair.left().len();
    if (pair.can_merge()) {
      pair.merge().deallocate(pages);
      return child_is_right ? child_fix::merged_into_left : child_fix::merged_right_in;
    }
    [[maybe_unused]] const bool stolen = child_is_right ? pair.steal_left() : pair.steal_right();
    assert(stolen && "a sibling too full to merge with can spare an entry");
    return child_is_right ? child_fix::took_from_left : child_fix::took_from_right;
  }
};

template <class T>
template <class Pages>
std::pair<typename T::key_type, typename T::mapped_type> node_stack_mut<T>::remove(
    const Pages& pages, std::size_t& length) noexcept {
  if (node().is_leaf()) {
    const leaf_node<T> bottom = leaf();
    if (bottom.keeps_its_minimum() || is_at_root()) {
      std::pair<K, V> entry = bottom.remove(index());
      --length;
      detach();
      return entry;
    }
  }
  auto [entry, changed] = take_out(pages, length);
  lower_and_refit_changed_fences(changed, pages);
  detach();
  return std::move(entry);
}

template <class T>
template <class Pages>
std::pair<typename T::key_type, typename T::mapped_type> node_stack_mut<T>::remove_and_go_to_next(
    const Pages& pages, std::size_t& length) noexcept {
  if (node().is_leaf()) {
    const leaf_node<T> bottom = leaf();
    if (bottom.keeps_its_minimum() || is_at_root()) {
      const std::size_t at = index();
      std::pair<K, V> entry = bottom.remove(at);
      --length;
      go_to_entry_at_slot(at + 1);
      return entry;
    }
  }
  auto [entry, changed] = take_out(pages, length);
  for (std::size_t at = 0; at < depth(); ++at) {
    changed.indexes[at] = stack_.index_at(at);
  }
  lower_and_refit_changed_fences(changed, pages);
  // The next entry's slot in its leaf, as the fixing tracked it through its
  // merges and steals, and the edges down to that leaf.
  start_at_root();
  for (std::size_t at = 0; at < changed.leaf_depth; ++at) {
    set_index(changed.indexes[at]);
    descend();
  }
  go_to_entry_at_slot(changed.indexes[changed.leaf_depth]);
  return std::move(entry);
}

template <class T>
void node_stack_mut<T>::go_to_entry_at_slot(std::size_t slot) noexcept {
  if (slot >= 1 && slot <= leaf().len()) {
    set_index(slot - 1);
    return;
  }
  while (!is_at_root()) {
    const internal_node<T> parent = ascend();
    const std::size_t edge = index();
    if (slot > 0 && edge < parent.len()) {
      return;
    }
    if (slot == 0 && edge > 0) {
      set_index(edge - 1);
      return;
    }
  }
  detach();
}

template <class T>
template <class Pages>
auto node_stack_mut<T>::take_out(const Pages& pages, std::size_t& length) noexcept
    -> std::pair<std::pair<K, V>, changed_fences> {
  const std::size_t entry_depth = depth();
  const std::size_t entry_index = index();
  if (node().is_leaf()) {
    std::pair<K, V> entry = leaf().remove(entry_index);
    changed_fences changed = fix_up_the_stack(entry_index + 1, std::nullopt, pages, [](internal_node<T>) {});
    --length;
    return {std::move(entry), std::move(changed)};
  }
  refit_left_of_separator();
  set_index(entry_index + 1);
  descend_to_leaf<node_end::first>();
  std::optional<head_succession<T>> succession = leaf().take_head_and_first_key();
  assert(succession && "the leaf right of a separator has keys");
  std::optional<separator<T>> successor(std::move(succession->successor));
  std::optional<separator<T>> replaced;
  changed_fences changed = fix_up_the_stack(0, entry_depth, pages, [&](internal_node<T> node) {
    replaced.emplace(node.replace_separator_from_below(entry_index, std::move(*successor)));
  });
  assert(replaced && "the node stack goes up past the separator");
  K key = [&] {
    if constexpr (T::has_inline_keys) {
      return std::move(*succession->removed_key);
    } else {
      return std::move(std::move(*replaced).take_key().key);
    }
  }();
  --length;
  return {{std::move(key), std::move(succession->value)}, std::move(changed)};
}

template <class T>
template <class Pages, class AtSeparator>
auto node_stack_mut<T>::fix_up_the_stack(std::size_t next_slot, std::optional<std::size_t> must_reach,
                                         const Pages& pages, AtSeparator&& at_separator) noexcept
    -> changed_fences {
  changed_fences changed(depth(), next_slot, must_reach);
  bool child_is_underfull = leaf().is_underfull();
  while (!is_at_root() && (child_is_underfull || (must_reach && depth() > *must_reach))) {
    const internal_node<T> node = ascend();
    const std::size_t at = depth();
    const std::size_t edge = index();
    changed.indexes[at] = edge;
    if (must_reach == at) {
      at_separator(node);
    }
    if (child_is_underfull) {
      std::size_t left_len = 0;
      const child_fix fix = node.has_leaf_children()
                                ? underfull_child<T, leaf_node<T>>::fix(node, edge, pages, left_len)
                                : underfull_child<T, internal_node<T>>::fix(node, edge, pages, left_len);
      const bool merged = fix == child_fix::merged_into_left || fix == child_fix::merged_right_in;
      changed.record(at, fix, left_len);
      child_is_underfull = merged && node.is_underfull();
    }
  }
  return changed;
}

template <class T>
template <class Pages>
void node_stack_mut<T>::lower_and_refit_changed_fences(changed_fences& changed, const Pages& pages) noexcept {
  if (lower_an_emptied_root(changed.reached_root(), pages)) {
    changed.lower();
  }
  if (!changed.top_depth) {
    return;
  }
  const std::size_t top_depth = *changed.top_depth;
  while (depth() + 1 < changed.leaf_depth) {
    set_index(changed.indexes[depth()]);
    descend();
  }
  while (true) {
    const std::size_t at = depth();
    const std::size_t child = changed.indexes[at];
    set_index(child);
    refit_children_to_fences(child, changed.donors[at]);
    if (at == top_depth) {
      return;
    }
    ascend();
  }
}

}  // namespace bbtree::detail
