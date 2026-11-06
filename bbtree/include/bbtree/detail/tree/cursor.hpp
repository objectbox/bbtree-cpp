// A position at an entry of a tree, or past its last: the core of the maps'
// iterators (D23, PORTING.md P7).
//
// A cursor holds its entry's key and value addresses, so dereferencing reads
// no node. Every entry has a key slot of its own, a leaf's or a separator's
// (D17, D49), so two cursors are at one entry exactly when their key
// addresses are equal; past the last entry the address is null.
//
// Within a leaf, a step adds the slots' strides to both addresses and checks
// against the leaf's key end, as the Rust walks' cached leaf runs do (D33,
// D42). Crossing a leaf boundary goes up to the nearest separator, whose value
// is the next leaf's head (D17), and down to the next leaf, along a node stack
// from the root (D11, D24). A cursor that a descent made (`begin`, bounds)
// has its node stack at once; one that a lookup or an insert made, which kept
// no ancestors (D32, D37), gets it on its first crossing, by searching its
// key.
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <cassert>
#include <cstddef>

namespace bbtree::detail {

template <class T>
class cursor {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

 public:
  cursor() noexcept = default;

  // Past the last entry of `owner`.
  explicit cursor(const tree<T>* owner) noexcept : tree_(owner) {}

  // At `entry`, which a lookup or an insert found; its node stack comes on
  // its first crossing.
  cursor(const tree<T>* owner, found_entry<T> entry) noexcept : tree_(owner) { point_at(entry); }

  // At the entry a node stack of `owner` ends at, or past the last if it is
  // detached.
  cursor(const tree<T>* owner, const node_stack<T>& at) noexcept : tree_(owner), stack_(at) {
    if (stack_.is_attached()) {
      point_at_stack();
    }
  }

  // At the tree's first entry, or past the last for an empty tree.
  static cursor first(const tree<T>* owner) noexcept {
    cursor at(owner);
    if (owner->has_root()) {
      at.stack_.attach(owner->root());
      if (at.stack_.descend_to_first_key()) {
        at.point_at_stack();
      } else {
        at.stack_.detach();
      }
    }
    return at;
  }

  // At the first entry not ordered before `query` (`side` before), or after
  // it (`side` after): a range bound.
  template <class Q>
  static cursor beside(const tree<T>* owner, const Q& query, key_side side) noexcept {
    cursor at(owner);
    if (owner->has_root()) {
      at.stack_.attach(owner->root());
      at.stack_.search_to_edge(search_key<T, Q>(query), side, owner->shared());
      at.at_leaf_edge();
    }
    return at;
  }

  bool is_end() const noexcept { return key_ == nullptr; }
  K* key() const noexcept { return key_; }
  V* value() const noexcept { return value_; }
  const tree<T>* owner() const noexcept { return tree_; }
  friend bool operator==(const cursor& a, const cursor& b) noexcept { return a.key_ == b.key_; }

  // Steps to the next entry, or past the last.
  void next() noexcept {
    assert(!is_end() && "stepping past the end");
    if (key_end_ != nullptr) {
      key_ = advance(key_, T::leaf.keys.key_stride);
      value_ = advance(value_, T::leaf.value_stride);
      if (key_ != key_end_) {
        return;
      }
    }
    cross_forward();
  }

  // Steps to the previous entry; from past the last, to the last.
  void prev() noexcept {
    if (is_end()) {
      *this = last(tree_);
      return;
    }
    if (key_end_ != nullptr && key_ != key_begin()) {
      key_ = advance(key_, -static_cast<std::ptrdiff_t>(T::leaf.keys.key_stride));
      value_ = advance(value_, -static_cast<std::ptrdiff_t>(T::leaf.value_stride));
      return;
    }
    cross_backward();
  }

  // The node stack at the entry, attached first if the cursor has none; for
  // a removal at the cursor.
  const node_stack<T>& attached_stack() noexcept {
    attach();
    return stack_;
  }

 private:
  static cursor last(const tree<T>* owner) noexcept {
    cursor at(owner);
    if (owner->has_root()) {
      at.stack_.attach(owner->root());
      if (at.stack_.descend_to_last_key()) {
        at.point_at_stack();
      } else {
        at.stack_.detach();
      }
    }
    return at;
  }

  template <class P>
  static P* advance(P* pointer, std::ptrdiff_t bytes) noexcept {
    return reinterpret_cast<P*>(reinterpret_cast<std::byte*>(pointer) + bytes);
  }

  K* key_begin() const noexcept {
    return advance(key_end_, -static_cast<std::ptrdiff_t>(leaf_len_ * T::leaf.keys.key_stride));
  }

  // Points at `entry`: a leaf's key, with the leaf's run, or a separator.
  void point_at(found_entry<T> entry) noexcept {
    key_ = entry.key();
    value_ = entry.value();
    if (entry.node.is_leaf()) {
      const leaf_node<T> leaf(entry.node);
      leaf_len_ = leaf.len();
      key_end_ = leaf.key_slots_only().slot(leaf_len_);
    } else {
      key_end_ = nullptr;
    }
  }

  // Points at the entry the attached node stack ends at.
  void point_at_stack() noexcept { point_at({stack_.node(), stack_.index()}); }

  void detach_to_end() noexcept {
    key_ = nullptr;
    value_ = nullptr;
    key_end_ = nullptr;
    stack_.detach();
  }

  // From a node stack at a leaf edge: the entry right of it, which may be the
  // separator after the leaf, or past the last.
  void at_leaf_edge() noexcept {
    const leaf_node<T> leaf = stack_.leaf();
    if (stack_.index() < leaf.len()) {
      point_at_stack();
      return;
    }
    climb_to_separator_after();
  }

  // Attaches the node stack at the entry by searching its key, if it has
  // none: a cursor that a lookup made.
  void attach() noexcept {
    if (stack_.is_attached()) {
      sync_leaf_index();
      return;
    }
    stack_.attach(tree_->root());
    [[maybe_unused]] const bool found = stack_.search(search_key<T, K>(*key_), tree_->shared());
    assert(found && stack_.node() == entry_node() && "a cursor's key is found at its entry");
  }

  // The node the cursor's entry is in, as far as its addresses tell: only
  // for the debug check.
  node_ptr<T> entry_node() const noexcept { return stack_.node(); }

  // Brings an attached node stack's leaf index to the entry the in-leaf
  // steps reached.
  void sync_leaf_index() noexcept {
    if (key_end_ != nullptr) {
      const std::size_t from_end = static_cast<std::size_t>(reinterpret_cast<std::byte*>(key_end_) -
                                                            reinterpret_cast<std::byte*>(key_)) /
                                   T::leaf.keys.key_stride;
      stack_.set_index(leaf_len_ - from_end);
    }
  }

  void cross_forward() noexcept {
    if (key_end_ != nullptr) {
      // Past the leaf's last entry: the leaf edge after it.
      key_ = advance(key_, -static_cast<std::ptrdiff_t>(T::leaf.keys.key_stride));
      attach();
      stack_.set_index(stack_.index() + 1);
      climb_to_separator_after();
      return;
    }
    attach();
    // At a separator: the first entry of the leftmost leaf right of it.
    stack_.set_index(stack_.index() + 1);
    stack_.template descend_to_leaf<node_end::first>();
    point_at_stack();
  }

  void cross_backward() noexcept {
    attach();
    if (stack_.ends_at_leaf()) {
      // Before the leaf's first entry: the separator left of it.
      for (std::size_t at = stack_.depth(); at-- > 0;) {
        if (stack_.index_at(at) > 0) {
          while (stack_.depth() > at) {
            stack_.ascend();
          }
          stack_.set_index(stack_.index() - 1);
          point_at_stack();
          return;
        }
      }
      assert(false && "stepping before the first entry");
      return;
    }
    // At a separator: the last entry of the rightmost leaf left of it.
    stack_.template descend_to_leaf<node_end::last>();
    stack_.set_index(stack_.index() - 1);
    point_at_stack();
  }

  // From a node stack at a leaf's last edge: up to the nearest separator
  // right of it, or past the last entry.
  void climb_to_separator_after() noexcept {
    for (std::size_t at = stack_.depth(); at-- > 0;) {
      if (stack_.index_at(at) < stack_.internal_at(at).len()) {
        while (stack_.depth() > at) {
          stack_.ascend();
        }
        point_at_stack();
        return;
      }
    }
    detach_to_end();
  }

  const tree<T>* tree_ = nullptr;
  K* key_ = nullptr;
  V* value_ = nullptr;
  // One past the last key of the leaf the entry is in; null at a separator.
  K* key_end_ = nullptr;
  std::size_t leaf_len_ = 0;
  node_stack<T> stack_;
};

}  // namespace bbtree::detail
