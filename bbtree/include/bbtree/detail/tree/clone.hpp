// Copying a tree node by node, so that the copy has the same shape, as std's
// `BTreeMap::clone` does: every inline key, prefix length and the shared
// prefix are copied, and no comparator or encoding runs. If a key's or
// value's copy throws, the part copied so far is taken apart.
#pragma once

#include <bbtree/detail/tree/teardown.hpp>
#include <bbtree/detail/tree/tree.hpp>

namespace bbtree::detail {

template <class T>
struct node_copies {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

  // A copy of `node` and the nodes under it, on plain pages.
  template <class Pages>
  static node_ptr<T> copy(node_ptr<T> node, const Pages& pages) {
    if (node.is_leaf()) {
      const leaf_node<T> leaf(node);
      const leaf_node<T> copied =
          leaf_node<T>::create(fresh_page<T, page_role::leaf>::allocate(pages), leaf.prefix_len());
      try {
        if (leaf.has_head()) {
          std::optional<K> key;
          if constexpr (T::has_inline_keys) {
            key.emplace(leaf.head_key());
          }
          copied.put_head(std::move(key), V(leaf.head_value()));
        }
        for (std::size_t index = 0; index < leaf.len(); ++index) {
          copied.insert_fit(index, windowed_key<K>{K(leaf.key(index)), leaf.window_at(index)},
                            V(leaf.value(index)));
        }
      } catch (...) {
        free(copied.node(), pages);
        throw;
      }
      return copied.node();
    }
    const internal_node<T> internal(node);
    const node_ptr<T> first = copy(internal.edge(0), pages);
    internal_node<T> copied;
    try {
      copied = internal_node<T>::create(fresh_page<T, page_role::internal>::allocate(pages), first,
                                        internal.prefix_len());
    } catch (...) {
      free(first, pages);
      throw;
    }
    try {
      for (std::size_t index = 0; index < internal.len(); ++index) {
        separator<T> middle = [&] {
          if constexpr (T::has_inline_keys) {
            return separator<T>(internal.window_at(index));
          } else {
            return separator<T>(windowed_key<K>{K(internal.separator_key(index)), internal.window_at(index)});
          }
        }();
        const node_ptr<T> child = copy(internal.edge(index + 1), pages);
        copied.insert_fit(index, std::move(middle), child);
      }
    } catch (...) {
      free(copied.node(), pages);
      throw;
    }
    return copied.node();
  }

  template <class Pages>
  static void free(node_ptr<T> node, const Pages& pages) noexcept {
    teardown<T>::empty_node(node, pages);
    if (node.is_leaf()) {
      std::move(leaf_node<T>(node).free()).deallocate(pages);
    } else {
      std::move(internal_node<T>(node).free()).deallocate(pages);
    }
  }
};

template <class T>
template <class Pages>
tree<T> tree<T>::clone(const Pages& pages) const {
  tree copied;
  if (!has_root()) {
    return copied;
  }
  const node_ptr<T> top = node_copies<T>::copy(root(), pages);
  try {
    copied.adopt_top(top, shared_, pages);
  } catch (...) {
    node_copies<T>::free(top, pages);
    throw;
  }
  return copied;
}

}  // namespace bbtree::detail
