// Two adjacent children of an internal node, of either kind, and the separator
// between them. A removal that leaves a child underfull fixes it here, as do
// cuts and builds that leave a border short: they merge the two, or move one
// entry from one to the other through the separator (D22).
#pragma once

#include <bbtree/detail/node/internal.hpp>

#include <cassert>
#include <cstddef>
#include <utility>

namespace bbtree::detail {

template <class T, class Child>
class siblings {
 public:
  // Separator `index` of `parent` and the children beside it, of kind
  // `Child`; the right one has its first trailing slot (a leaf's head).
  siblings(internal_node<T> parent, std::size_t index) noexcept
      : parent_(parent), index_(index), left_(parent.edge(index)), right_(parent.edge(index + 1)) {
    assert(index < parent.len() && "separator index out of bounds");
  }

  internal_node<T> parent() const noexcept { return parent_; }
  std::size_t index() const noexcept { return index_; }
  Child left() const noexcept { return left_; }
  Child right() const noexcept { return right_; }

  bool can_merge() const noexcept { return Child::fit_merged(left_.len(), right_.len()); }

  // Moves the separator and all of the right child into the left one, which
  // takes the parent's prefix length, filled from the separator's window;
  // removes the separator and the right edge from the parent. Returns the
  // right child's emptied page. The caller refits the merged child's prefix.
  typename Child::page_type merge() const noexcept {
    assert(can_merge() && "the children don't fit in one node with their separator");
    const window fill = parent_.window_at(index_);
    separator<T> middle = parent_.remove_with_right_child(index_).first;
    left_.move_prefix_back_to(fill);
    right_.move_prefix_back_to(fill);
    Child::append(left_, fill, std::move(middle), right_);
    return right_.free();
  }

  // Rotates one entry rightwards: the left child's last key goes up, the
  // separator comes down to the right child's front. False if the left
  // child has none or the right one is full.
  bool steal_left() const noexcept {
    if (left_.len() == 0 || right_.len() == Child::capacity) {
      return false;
    }
    const window fill = parent_.window_at(index_);
    right_.move_prefix_back_to(fill);
    Child::steal_left(parent_, index_, left_, right_, fill);
    return true;
  }

  // Rotates one entry leftwards, mirrored.
  bool steal_right() const noexcept {
    if (right_.len() == 0 || left_.len() == Child::capacity) {
      return false;
    }
    const window fill = parent_.window_at(index_);
    left_.move_prefix_back_to(fill);
    Child::steal_right(parent_, index_, left_, right_, fill);
    return true;
  }

 private:
  internal_node<T> parent_;
  std::size_t index_;
  Child left_;
  Child right_;
};

}  // namespace bbtree::detail
