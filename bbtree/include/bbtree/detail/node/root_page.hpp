// The root page: the root node, of either kind, in room for the larger. A
// tree keeps its root page for life; the root node's kind changes in place
// (D52). Raising the tree moves the root node to a fresh page of its kind and
// makes the root internal above it; lowering moves the root's only child up
// and frees the child's page. The tree's pointer to the page is the root's
// edge: its tag tells the root's kind.
#pragma once

#include <bbtree/detail/node/internal.hpp>

#include <cassert>
#include <cstddef>
#include <utility>

namespace bbtree::detail {

template <class T>
class root_page {
 public:
  using page_type = fresh_page<T, page_role::root>;

  root_page() noexcept = default;
  explicit root_page(node_ptr<T> node) noexcept : node_(node) {}

  // A new tree's root page: an empty leaf.
  static root_page create(page_type&& page) noexcept {
    return root_page(node_ptr<T>::template initialize<true>(std::move(page).release()));
  }

  // Makes `page` the root page for `top`, a tree's top node on a plain page,
  // moving it there; returns it with `top`'s emptied page.
  static std::pair<root_page, any_page<T>> from_node(page_type&& page, node_ptr<T> top) noexcept {
    std::byte* into = std::move(page).release();
    return move_in(into, top);
  }

  node_ptr<T> node() const noexcept { return node_; }
  explicit operator bool() const noexcept { return static_cast<bool>(node_); }

  // Makes the tree a level taller: the root node moves to a fresh page of
  // its kind, under a new internal root at `shared_len`.
  template <class Pages>
  root_page raise(const Pages& pages, std::size_t shared_len) const {
    node_ptr<T> child;
    if (node_.is_leaf()) {
      auto page = fresh_page<T, page_role::leaf>::allocate(pages);
      child = leaf_node<T>(node_).move_to(std::move(page).release()).node();
    } else {
      auto page = fresh_page<T, page_role::internal>::allocate(pages);
      child = internal_node<T>(node_).move_to(std::move(page).release()).node();
    }
    const internal_node<T> root(node_ptr<T>::template initialize<false>(node_.page()));
    root.put_only_child(child);
    root.set_empty_prefix_len(shared_len);
    return root_page(root.node());
  }

  // Makes the tree a level shorter: the root's only child moves up into the
  // root page; returns the page's new handle and the child's emptied page.
  std::pair<root_page, any_page<T>> lower() const noexcept {
    assert(!node_.is_leaf() && "lowering a leaf root");
    const internal_node<T> root(node_);
    assert(root.len() == 0 && "lowering a root with separators");
    return move_in(node_.page(), root.edge(0));
  }

  // Gives the page back; the root node holds nothing anymore.
  page_type free() const noexcept { return page_type::adopt(node_.page()); }

 private:
  static std::pair<root_page, any_page<T>> move_in(std::byte* into, node_ptr<T> node) noexcept {
    if (node.is_leaf()) {
      const leaf_node<T> leaf(node);
      return {root_page(leaf.move_to(into).node()), any_page<T>(leaf.free())};
    }
    const internal_node<T> internal(node);
    return {root_page(internal.move_to(into).node()), any_page<T>(internal.free())};
  }

  node_ptr<T> node_;
};

}  // namespace bbtree::detail
