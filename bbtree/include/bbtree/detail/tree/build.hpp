// Building a tree from entries in ascending key order, comparing no keys
// (D25): each leaf fills before the next starts beside it, so every node is
// full but along the right border, which `finish` tops up past the minimum
// from the full left siblings (`borders.hpp`). No node is split in half, as
// inserting the same entries one by one would do.
//
// Right-border nodes have no right fence, so their prefix is what every key
// shares (D50); a key that shares less shrinks that first. A full node closes
// once the next leaf starts after it and gets what its fences share as its
// prefix.
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <cstddef>
#include <utility>

namespace bbtree::detail {

template <class T, class A>
class tree_builder {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

 public:
  explicit tree_builder(const page_source<A>& pages) noexcept : pages_(pages) {}
  tree_builder(const tree_builder&) = delete;
  tree_builder& operator=(const tree_builder&) = delete;
  ~tree_builder() { tree_.destroy(pages_); }

  // Appends an entry whose key is greater than every key before it.
  void push(K&& key, V&& value) {
    if (!tree_.has_root()) {
      tree_.make_root(pages_);
      border_leaf_ = leaf_node<T>(tree_.root());
    }
    const prefixed_key<K&&> prefixed = tree_.template shrink_shared_prefix_for<K&&>(std::move(key));
    const std::size_t shared_len = tree_.shared().len();
    if (border_leaf_.len() < leaf_node<T>::capacity) {
      border_leaf_.insert_fit(border_leaf_.len(), prefixed.template window_at<T>(shared_len),
                              std::move(value));
    } else {
      start_leaf(prefixed.template window_at<T>(shared_len), std::move(value));
    }
    ++length_;
  }

  // The tree, with its right border fixed, and its length.
  std::pair<tree<T>, std::size_t> finish() && noexcept {
    if (tree_.has_root()) {
      tree_.template fix_border<node_end::last>(pages_);
      tree_.template refit_border_siblings<node_end::last>();
    }
    return {std::move(tree_), std::exchange(length_, 0)};
  }

 private:
  // Starts a new leaf, the separator's head: the separator goes into the
  // lowest border node with room, under new border nodes for each full one,
  // raising the tree if every one is full. Its pages come first.
  void start_leaf(windowed_key<K>&& separator_key, V&& value) {
    reserved_pages<A> reserved(pages_);
    {
      std::size_t full = 0;
      std::size_t depth = 0;
      for (node_ptr<T> node = tree_.root(); !node.is_leaf(); node = internal_node<T>(node).edge(node.len())) {
        full = node.len() == internal_node<T>::capacity ? full + 1 : 0;
        ++depth;
      }
      const bool raises = full == depth;
      reserved.reserve(page_kind::leaf, T::leaf.page, depth == 0 ? 2 : 1);
      reserved.reserve(page_kind::internal, T::internal.page, full + (raises && depth > 0 ? 1 : 0));
    }
    const std::size_t shared_len = tree_.shared().len();
    const leaf_node<T> leaf =
        leaf_node<T>::create(fresh_page<T, page_role::leaf>::allocate(reserved), shared_len);
    separator<T> middle = leaf.put_separator_head(std::move(separator_key), std::move(value));
    node_ptr<T> child = leaf.node();
    node_stack_mut<T> stack(tree_);
    stack.start_at_root();
    stack.stack().descend_to_last_key();
    while (true) {
      const internal_node<T> node = stack.ascend_raising(reserved);
      const std::size_t len = node.len();
      if (len < internal_node<T>::capacity) {
        node.insert_fit(len, std::move(middle), child);
        break;
      }
      child =
          internal_node<T>::create(fresh_page<T, page_role::internal>::allocate(reserved), child, shared_len)
              .node();
    }
    border_leaf_ = leaf;
    // The nodes the new leaf closed fit their prefixes to their fences.
    const std::size_t edge = stack.index();
    stack.template refit_border_below<node_end::last>(edge);
    stack.refit_children_to_fences(edge, std::nullopt);
  }

  page_source<A> pages_;
  tree<T> tree_;
  leaf_node<T> border_leaf_;
  std::size_t length_ = 0;
};

}  // namespace bbtree::detail
