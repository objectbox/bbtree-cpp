// A tree's borders after a bulk change (D25), and cutting a tree in two.
//
// - `tree::fix_border` brings the nodes along one edge of a tree up to their
//   minimum (D22), from the top down. A tree cut in two needs it along both
//   sides of the cut; a tree built from the left (`build.hpp`) along its
//   right edge, where nodes may hold any number of keys down to none. It
//   reads no key: the siblings that give the border entries keep their
//   prefixes, which still fit their narrower fences, until
//   `refit_border_siblings` fits them once the operation is done.
// - `tree::split_off_at` cuts a tree at a leaf edge: up the node stack from
//   there, each node keeps what lies left of it, and what lies right goes into
//   a new node, after the part cut off below. The node stack's nodes become
//   both trees' borders, so their prefixes first move back to the root's.
#pragma once

#include <bbtree/detail/node/siblings.hpp>
#include <bbtree/detail/tree/tree.hpp>

#include <cstddef>

namespace bbtree::detail {

template <class T>
struct border_fixes {
  // Brings the border child of `pair` to one key more than its minimum, by a
  // merge with its sibling or entries moved over from it, so that a merge
  // below leaves it at its minimum. Returns the border child.
  template <node_end End, class Child, class Pages>
  static Child stock(const siblings<T, Child>& pair, const Pages& pages) noexcept {
    const std::size_t target = Child::min_len + 1;
    auto border_len = [&] { return End == node_end::last ? pair.right().len() : pair.left().len(); };
    if (border_len() < target) {
      if (pair.can_merge()) {
        const Child merged = pair.left();
        pair.merge().deallocate(pages);
        return merged;
      }
      while (border_len() < target) {
        [[maybe_unused]] const bool moved = End == node_end::last ? pair.steal_left() : pair.steal_right();
        assert(moved && "a sibling too full to merge with can spare a key");
      }
    }
    return End == node_end::last ? pair.right() : pair.left();
  }

  static std::size_t count_entries(node_ptr<T> node) noexcept {
    if (node.is_leaf()) {
      return node.len();
    }
    const internal_node<T> internal(node);
    std::size_t count = internal.len();
    for (std::size_t edge = 0; edge <= internal.len(); ++edge) {
      count += count_entries(internal.edge(edge));
    }
    return count;
  }
};

template <class T>
template <node_end End, class Pages>
void tree<T>::fix_border(const Pages& pages) noexcept {
  forget_rightmost_leaf();
  lower_while_root_empty(pages);
  if (!root().is_leaf()) {
    internal_node<T> node(root());
    while (true) {
      const std::size_t len = node.len();
      assert(len > 0 && "a node on the border holds a key once stocked");
      const std::size_t separator_index = End == node_end::last ? len - 1 : 0;
      if (node.has_leaf_children()) {
        border_fixes<T>::template stock<End>(siblings<T, leaf_node<T>>(node, separator_index), pages);
        break;
      }
      node =
          border_fixes<T>::template stock<End>(siblings<T, internal_node<T>>(node, separator_index), pages);
    }
  }
  lower_while_root_empty(pages);
}

template <class T>
template <class Pages>
void tree<T>::lower_while_root_empty(const Pages& pages) noexcept {
  while (!root().is_leaf() && root().len() == 0) {
    lower().deallocate(pages);
  }
}

// Fits the prefix of the node beside each node along the `End` border to its
// fences, from the root down: the siblings `fix_border` took entries from.
template <class T>
template <node_end End>
void tree<T>::refit_border_siblings() noexcept {
  if constexpr (T::has_inline_keys) {
    node_stack_mut<T> stack(*this);
    stack.start_at_root();
    while (!stack.node().is_leaf()) {
      const std::size_t len = stack.node().len();
      assert(len > 0 && "a node on a fixed border holds a key");
      stack.refit_children_to_fences(End == node_end::last ? len - 1 : 1, std::nullopt);
      stack.set_index(End == node_end::last ? stack.node().len() : 0);
      stack.descend();
    }
  }
}

// Fits the prefixes of the nodes below the bottom node's child at `edge`,
// down that child's `End` border, from the leaf up; the child itself is the
// caller's. Ends at the bottom node again, at `edge`.
template <class T>
template <node_end End>
void node_stack_mut<T>::refit_border_below(std::size_t edge) noexcept {
  if constexpr (T::has_inline_keys) {
    const std::size_t start_depth = depth();
    set_index(edge);
    descend_to_leaf<End>();
    ascend();
    while (depth() > start_depth) {
      refit_children_to_fences(index(), std::nullopt);
      ascend();
    }
  } else {
    (void)edge;
  }
}

// Moves the prefix of every node on the node stack back to the root's, from
// the root down, each filled from its parent's separator beside the stack:
// for a cut, whose nodes become the borders of two trees. Reads no key.
template <class T>
void node_stack_mut<T>::move_prefixes_back_to_root() noexcept {
  if constexpr (T::has_inline_keys) {
    for (std::size_t at = 0; at < depth(); ++at) {
      const std::optional<window> fill = stack_.internal_at(at).fence_window(stack_.index_at(at));
      assert(fill && "a node on a node stack down holds a key");
      const node_ptr<T> below = stack_.node_at(at + 1);
      if (below.is_leaf()) {
        leaf_node<T>(below).move_prefix_back_to(*fill);
      } else {
        internal_node<T>(below).move_prefix_back_to(*fill);
      }
    }
  }
}

// Cuts every node on the node stack at its edge; returns the top of the
// tree right of the cut, on plain pages.
template <class T>
template <class Pages>
node_ptr<T> node_stack_mut<T>::cut(const Pages& pages) {
  move_prefixes_back_to_root();
  node_ptr<T> right = leaf().cut_off_at(index(), fresh_page<T, page_role::leaf>::allocate(pages)).node();
  while (!is_at_root()) {
    const internal_node<T> node = ascend();
    right = node.cut_off_at(index(), right, fresh_page<T, page_role::internal>::allocate(pages)).node();
  }
  return right;
}

// Cuts the tree at the leaf edge on `side` of `query`: the entries from there
// on make the returned tree; this one keeps the rest. Both trees' borders are
// fixed and refit; `length` becomes this tree's length, and the returned
// length is the other's.
template <class T>
template <class Q, class Pages>
std::pair<tree<T>, std::size_t> tree<T>::split_off_at(const Q& query, key_side side, const Pages& pages,
                                                      std::size_t& length) {
  tree upper;
  if (!has_root()) {
    return {std::move(upper), 0};
  }
  // The cut's pages, one per level, are taken first: a refused one throws
  // before anything changes.
  reserved_pages<typename Pages::allocator_type> reserved(pages);
  reserved.reserve(page_kind::leaf, T::leaf.page, 1);
  reserved.reserve(page_kind::internal, T::internal.page, leaf_depth());
  reserved.reserve(page_role_traits<T>::kind(page_role::root), T::root_page, 1);
  node_stack_mut<T> stack(*this);
  stack.start_at_root();
  stack.stack().search_to_edge(search_key<T, Q>(query), side, shared_);
  const node_ptr<T> top = stack.cut(reserved);
  upper.adopt_top(top, shared_, reserved);
  fix_border<node_end::last>(pages);
  upper.template fix_border<node_end::first>(pages);
  std::size_t upper_length = 0;
  if (leaf_depth() < upper.leaf_depth()) {
    const std::size_t lower_length = border_fixes<T>::count_entries(root());
    upper_length = length - lower_length;
  } else {
    upper_length = border_fixes<T>::count_entries(upper.root());
  }
  length -= upper_length;
  refit_border_siblings<node_end::last>();
  upper.template refit_border_siblings<node_end::first>();
  return {std::move(upper), upper_length};
}

}  // namespace bbtree::detail
