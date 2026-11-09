// Copying a tree node by node, so that the copy has the same shape, as std's
// `BTreeMap::clone` does: every inline key, prefix length and the shared
// prefix are copied, and no comparator or encoding runs. If a key's or
// value's copy throws, or a page is refused, the part copied so far is taken
// apart.
//
// Where keys and values are trivially copyable, a node's copy is its page's
// bytes, an internal node's with its edges set to its children's copies, and
// the root's go straight into the copy's root page: nothing is copied entry
// by entry.
#pragma once

#include <bbtree/detail/tree/teardown.hpp>
#include <bbtree/detail/tree/tree.hpp>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace bbtree::detail {

template <class T>
struct node_copies {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

  // Whether a node's bytes are its copy: a leaf holds nothing but its keys,
  // its values and what the node code derives from them.
  static constexpr bool copies_pages = std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V> &&
                                       !T::has_leaf_annex && !T::latched && !T::in_file;

  // A copy of `node` and the nodes under it, on plain pages.
  template <class Pages>
  static node_ptr<T> copy(node_ptr<T> node, const Pages& pages) {
    if constexpr (copies_pages) {
      return node.is_leaf() ? copy_on_fresh_page<page_role::leaf>(node, pages)
                            : copy_on_fresh_page<page_role::internal>(node, pages);
    }
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

  // A copy of `node`, as its bytes, on a fresh page of role `Role`: its own
  // kind's, or the root's; the nodes under it on plain pages.
  template <page_role Role, class Pages>
  static node_ptr<T> copy_on_fresh_page(node_ptr<T> node, const Pages& pages)
    requires copies_pages
  {
    std::byte* page = fresh_page<T, Role>::allocate(pages).release();
    // The role tells the node's kind, but for the root's.
    const bool is_leaf = Role == page_role::leaf || (Role == page_role::root && node.is_leaf());
    assert(is_leaf == node.is_leaf() && "a node copied onto a page of its kind");
    if (is_leaf) {
      std::memcpy(page, node.page(), T::leaf.page.size);
      return node_ptr<T>::leaf_at(page);
    }
    std::memcpy(page, node.page(), T::internal.page.size);
    const internal_node<T> internal(node);
    const internal_node<T> copied(node_ptr<T>::internal_at(page));
    std::size_t edge = 0;
    try {
      for (; edge <= internal.len(); ++edge) {
        *copied.edge_slots().slot(edge) = copy(internal.edge(edge), pages);
      }
    } catch (...) {
      // The edges from `edge` on still lead to the original's children.
      while (edge-- > 0) {
        free(copied.edge(edge), pages);
      }
      std::move(fresh_page<T, Role>::adopt(page)).deallocate(pages);
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
  if constexpr (node_copies<T>::copies_pages) {
    copied.root_ = root_page<T>(node_copies<T>::template copy_on_fresh_page<page_role::root>(root(), pages));
    copied.shared_ = shared_;
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
