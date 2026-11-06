// Inserting into a tree.
//
// An insert first tries the last leaf a split remembered (D45): a key greater
// than its last appends there directly while the leaf has room. Otherwise it
// descends without a node stack, as a lookup does (D37): an existing key, or
// a leaf with room, needs no ancestors. A full leaf takes an outlined second
// search that records the node stack, takes the split's pages from the
// allocator first, and splits up the stack. A root that splits raises the
// tree. Once the entry is in and counted, the split halves refit their
// prefixes, and the stack goes back down to the new entry.
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <cstddef>
#include <utility>

namespace bbtree::detail {

template <class T>
bool tree<T>::appends_to_rightmost(const K& key) noexcept {
  if (!rightmost_leaf_) {
    return false;
  }
  const leaf_node<T> leaf(rightmost_leaf_);
  rightmost_leaf_ = node_ptr<T>();
  const std::size_t len = leaf.len();
  if (len >= leaf_node<T>::capacity || len == 0) {
    return false;
  }
  if (search_key<T, K>(key).cmp_full(leaf.key(len - 1)) <= 0) {
    return false;
  }
  return shared_.template is_shared_by<T>(key);
}

template <class T>
template <class A, class MakeValue>
insertion<T> tree<T>::insert(K&& key, MakeValue&& make_value, bool assign, const page_source<A>& pages,
                             std::size_t& length) {
  if (const node_ptr<T> remembered = rightmost_leaf_; appends_to_rightmost(key)) {
    const leaf_node<T> leaf(remembered);
    V value = make_value();
    const std::size_t index = leaf.len();
    leaf.insert_fit(index, prefixed_key<K>(std::move(key)).template window_at<T>(leaf.prefix_len()),
                    std::move(value));
    rightmost_leaf_ = remembered;
    ++length;
    return {{remembered, index}, true};
  }
  if (!has_root()) {
    make_root(pages);
  }
  prefixed_key<K> prefixed = shrink_shared_prefix_for(std::move(key));
  forget_rightmost_leaf();

  // The descent without a node stack, as a lookup's.
  const search_key<T, K> query(prefixed.key());
  node_ptr<T> node = root();
  while (!node.is_leaf()) {
    const internal_node<T> internal(node);
    const search_result found = internal.search(query);
    if (found.found) {
      const found_entry<T> entry{node, found.index};
      if (assign) {
        *entry.value() = make_value();
      }
      return {entry, false};
    }
    node = internal.edge(found.index);
  }
  const leaf_node<T> leaf(node);
  const search_result found = leaf.search(query);
  if (found.found) {
    const found_entry<T> entry{node, found.index};
    if (assign) {
      *entry.value() = make_value();
    }
    return {entry, false};
  }
  if (leaf.len() < leaf_node<T>::capacity) {
    V value = make_value();
    leaf.insert_fit(found.index, std::move(prefixed).template window_at<T>(leaf.prefix_len()),
                    std::move(value));
    ++length;
    return {{node, found.index}, true};
  }
  return insert_with_stack(std::move(prefixed), std::forward<MakeValue>(make_value), assign, pages, length);
}

// The insert into a full leaf: searches again, recording the node stack, takes
// the split's pages, then splits.
template <class T>
template <class A, class MakeValue>
[[gnu::noinline]] insertion<T> tree<T>::insert_with_stack(prefixed_key<K>&& key, MakeValue&& make_value,
                                                          bool assign, const page_source<A>& pages,
                                                          std::size_t& length) {
  node_stack_mut<T> stack(*this);
  if (stack.search(search_key<T, K>(key.key()))) {
    const found_entry<T> entry = stack.found();
    if (assign) {
      *entry.value() = make_value();
    }
    return {entry, false};
  }
  // A split takes a leaf page, one internal page per full internal node
  // above it, and one more for a root that moves down: all before changing
  // anything (D8).
  reserved_pages<A> reserved(pages);
  if (stack.leaf().len() == leaf_node<T>::capacity) {
    const std::size_t depth = stack.depth();
    std::size_t full_internals = 0;
    std::size_t at = depth;
    while (at-- > 0 && stack.stack().internal_at(at).len() == internal_node<T>::capacity) {
      ++full_internals;
    }
    const bool raises = full_internals == depth;
    reserved.reserve(page_kind::leaf, T::leaf.page, depth == 0 ? 2 : 1);
    reserved.reserve(page_kind::internal, T::internal.page, full_internals + (raises && depth > 0 ? 1 : 0));
  }
  V value = make_value();
  stack.insert_prefixed(std::move(key), std::move(value), reserved, length);
  stack.finish_insertion();
  return {stack.found(), true};
}

template <class T>
template <class Pages>
void node_stack_mut<T>::insert_prefixed(prefixed_key<K>&& key, V&& value, const Pages& pages,
                                        std::size_t& length) {
  const leaf_node<T> bottom = leaf();
  assert(index() <= bottom.len() && "edge index out of bounds");
  if (bottom.len() < leaf_node<T>::capacity) {
    bottom.insert_fit(index(), std::move(key).template window_at<T>(bottom.prefix_len()), std::move(value));
    ++length;
  } else {
    insert_splitting(std::move(key), std::move(value), pages, length);
  }
}

// Splits the full leaf and carries each split up the node stack: every node
// takes its child's separator and right half, and splits in turn if full.
template <class T>
template <class Pages>
[[gnu::noinline]] void node_stack_mut<T>::insert_splitting(prefixed_key<K>&& key, V&& value,
                                                           const Pages& pages, std::size_t& length) {
  const leaf_node<T> bottom = leaf();
  auto split = bottom.split_insert(index(), std::move(key).template window_at<T>(bottom.prefix_len()),
                                   std::move(value), fresh_page<T, page_role::leaf>::allocate(pages));
  std::size_t indexes_in_halves[max_depth + 1];
  std::size_t separator_indexes[max_depth + 1];
  indexes_in_halves[0] = split.inserted.index;
  separator<T> middle = std::move(split.middle);
  node_ptr<T> right = split.right.node();
  bool entry_is_right = split.inserted.right;
  std::size_t climbed = 0;
  while (true) {
    const internal_node<T> node = ascend_raising(pages);
    ++climbed;
    const std::size_t edge = index();
    const std::size_t beside = entry_is_right ? 1 : 0;
    if (node.len() < internal_node<T>::capacity) {
      node.insert_fit(edge, std::move(middle), right);
      separator_indexes[climbed] = edge;
      set_index(edge + beside);
      break;
    }
    auto upper = node.split_insert(edge, std::move(middle), right,
                                   fresh_page<T, page_role::internal>::allocate(pages));
    separator_indexes[climbed] = upper.inserted.index;
    indexes_in_halves[climbed] = upper.inserted.index + beside;
    middle = std::move(upper.middle);
    right = upper.right.node();
    entry_is_right = upper.inserted.right;
  }
  const std::size_t levels_climbed = climbed;
  for (std::size_t level = levels_climbed; level-- > 0;) {
    descend();
    set_index(indexes_in_halves[level]);
  }
  ++length;
  refit_split_halves(separator_indexes, levels_climbed);
}

}  // namespace bbtree::detail
