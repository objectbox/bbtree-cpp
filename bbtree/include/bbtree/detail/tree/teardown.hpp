// Taking a tree apart: every entry dropped once, every page given back. A
// post-order walk, a level at a time down at most `max_depth` levels; leaves
// of entries without destructors are freed without visiting their slots. A
// leaf's annex is released to the map's allocator (D60).
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <type_traits>

namespace bbtree::detail {

template <class T>
struct teardown {
  using K = typename T::key_type;
  using V = typename T::mapped_type;
  static constexpr bool drops_entries =
      !std::is_trivially_destructible_v<K> || !std::is_trivially_destructible_v<V>;

  // Drops what `node` holds and everything under it, and frees the pages
  // under it; `node`'s own page is the caller's to give back.
  template <class Pages>
  static void empty_node(node_ptr<T> node, const Pages& pages) noexcept {
    if (node.is_leaf()) {
      const leaf_node<T> leaf(node);
      if constexpr (drops_entries) {
        for (std::size_t index = 0; index < leaf.len(); ++index) {
          leaf.key_slots_only().destroy(index);
          leaf.value_slots().destroy(index);
        }
        if (leaf.has_head()) {
          leaf.head_and_value_slots().destroy(0);
          if constexpr (T::has_inline_keys) {
            leaf.head_key_slot().destroy(0);
          }
        }
      }
      if constexpr (T::has_leaf_annex) {
        leaf.release_annex(pages.allocator());
      }
      return;
    }
    const internal_node<T> internal(node);
    for (std::size_t edge = 0; edge <= internal.len(); ++edge) {
      const node_ptr<T> child = internal.edge(edge);
      empty_node(child, pages);
      if (child.is_leaf()) {
        std::move(leaf_node<T>(child).free()).deallocate(pages);
      } else {
        std::move(internal_node<T>(child).free()).deallocate(pages);
      }
    }
    if constexpr (!T::has_inline_keys && !std::is_trivially_destructible_v<K>) {
      for (std::size_t index = 0; index < internal.len(); ++index) {
        internal.separator_key_slots().destroy(index);
      }
    }
  }
};

template <class T>
template <class Pages>
void tree<T>::destroy(const Pages& pages) noexcept {
  if (!has_root()) {
    return;
  }
  teardown<T>::empty_node(root(), pages);
  std::move(root_.free()).deallocate(pages);
  root_ = root_page<T>();
  rightmost_leaf_ = node_ptr<T>();
  shared_ = shared_prefix();
}

}  // namespace bbtree::detail
