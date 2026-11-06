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
//
// A step keeps the cursor's position in memory, since the outlined crossings
// take its node stack by address: range-for over 1M u64 entries takes 1.6 ns
// an entry with Clang, 1.3 with GCC. `visit_to_end` keeps the position in
// registers and runs a leaf's entries in one loop, as Rust's folds do: 0.9 ns
// (PORTING.md P16).
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <cassert>
#include <cstddef>

namespace bbtree::detail {

template <class T>
class cursor {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

  // Where the cursor is: its entry's key and value, and for an entry in a
  // leaf, the leaf's key end and length; past the last, all null.
  struct position {
    K* key = nullptr;
    V* value = nullptr;
    // One past the last key of the entry's leaf; null at a separator.
    K* key_end = nullptr;
    std::size_t leaf_len = 0;
  };

 public:
  cursor() noexcept = default;

  // Past the last entry of `owner`.
  explicit cursor(const tree<T>* owner) noexcept : tree_(owner) {}

  // At `entry`, which a lookup or an insert found; its node stack comes on
  // its first crossing.
  cursor(const tree<T>* owner, found_entry<T> entry) noexcept : tree_(owner), at_(position_of(entry)) {}

  // At the entry a node stack of `owner` ends at, or past the last if it is
  // detached.
  cursor(const tree<T>* owner, const node_stack<T>& at) noexcept : tree_(owner), stack_(at) {
    if (stack_.is_attached()) {
      at_ = position_of_stack(stack_);
    }
  }

  // At the tree's first entry, or past the last for an empty tree.
  static cursor first(const tree<T>* owner) noexcept {
    cursor at(owner);
    if (owner->has_root()) {
      at.stack_.attach(owner->root());
      if (at.stack_.descend_to_first_key()) {
        at.at_ = position_of_stack(at.stack_);
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
      at.at_ = at_leaf_edge(at.stack_);
    }
    return at;
  }

  bool is_end() const noexcept { return at_.key == nullptr; }
  K* key() const noexcept { return at_.key; }
  V* value() const noexcept { return at_.value; }
  const tree<T>* owner() const noexcept { return tree_; }
  friend bool operator==(const cursor& a, const cursor& b) noexcept { return a.at_.key == b.at_.key; }

  // Steps to the next entry, or past the last.
  void next() noexcept {
    assert(!is_end() && "stepping past the end");
    if (at_.key_end != nullptr) {
      at_.key = advance(at_.key, T::leaf.keys.key_stride);
      at_.value = advance(at_.value, T::leaf.value_stride);
      if (at_.key != at_.key_end) {
        return;
      }
    }
    at_ = cross_forward(tree_, stack_, at_);
  }

  // Runs `visit(key, value)` on every entry from the cursor's on, and leaves
  // the cursor past the last.
  template <class Visit>
  void visit_to_end(Visit& visit) {
    position at = at_;
    while (at.key != nullptr) {
      if (at.key_end != nullptr) {
        for (; at.key != at.key_end; at.key = advance(at.key, T::leaf.keys.key_stride),
                                     at.value = advance(at.value, T::leaf.value_stride)) {
          visit(*at.key, *at.value);
        }
      } else {
        visit(*at.key, *at.value);
      }
      at = cross_forward(tree_, stack_, at);
    }
    at_ = at;
  }

  // Steps to the previous entry; from past the last, to the last.
  void prev() noexcept {
    if (at_.key_end != nullptr && at_.key != key_begin(at_)) {
      at_.key = advance(at_.key, -static_cast<std::ptrdiff_t>(T::leaf.keys.key_stride));
      at_.value = advance(at_.value, -static_cast<std::ptrdiff_t>(T::leaf.value_stride));
      return;
    }
    at_ = cross_backward(tree_, stack_, at_);
  }

  // The node stack at the entry, attached first if the cursor has none; for
  // a removal at the cursor.
  const node_stack<T>& attached_stack() noexcept {
    attach(tree_, stack_, at_);
    return stack_;
  }

 private:
  template <class P>
  static P* advance(P* pointer, std::ptrdiff_t bytes) noexcept {
    return reinterpret_cast<P*>(reinterpret_cast<std::byte*>(pointer) + bytes);
  }

  static K* key_begin(const position& at) noexcept {
    return advance(at.key_end, -static_cast<std::ptrdiff_t>(at.leaf_len * T::leaf.keys.key_stride));
  }

  // The position at `entry`: a leaf's key, with the leaf's run, or a
  // separator.
  static position position_of(found_entry<T> entry) noexcept {
    position at{entry.key(), entry.value(), nullptr, 0};
    if (entry.node.is_leaf()) {
      const leaf_node<T> leaf(entry.node);
      at.leaf_len = leaf.len();
      at.key_end = leaf.key_slots_only().slot(at.leaf_len);
    }
    return at;
  }

  static position position_of_stack(const node_stack<T>& stack) noexcept {
    return position_of({stack.node(), stack.index()});
  }

  // From a node stack at a leaf edge: the entry right of it, which may be the
  // separator after the leaf, or past the last.
  static position at_leaf_edge(node_stack<T>& stack) noexcept {
    if (stack.index() < stack.leaf().len()) {
      return position_of_stack(stack);
    }
    return climb_to_separator_after(stack);
  }

  // Attaches the node stack at the entry by searching its key, if it has
  // none, or brings its leaf index to where the in-leaf steps went.
  static void attach(const tree<T>* owner, node_stack<T>& stack, const position& at) noexcept {
    if (stack.is_attached()) {
      if (at.key_end != nullptr) {
        const auto from_end = static_cast<std::size_t>(reinterpret_cast<std::byte*>(at.key_end) -
                                                       reinterpret_cast<std::byte*>(at.key)) /
                              T::leaf.keys.key_stride;
        stack.set_index(at.leaf_len - from_end);
      }
      return;
    }
    stack.attach(owner->root());
    [[maybe_unused]] const bool found = stack.search(search_key<T, K>(*at.key), owner->shared());
    assert(found && "a cursor's key is found at its entry");
  }

  [[gnu::noinline]] static position cross_forward(const tree<T>* owner, node_stack<T>& stack,
                                                  position at) noexcept {
    if (at.key_end != nullptr) {
      // Past the leaf's last entry: the leaf edge after it.
      at.key = advance(at.key, -static_cast<std::ptrdiff_t>(T::leaf.keys.key_stride));
      attach(owner, stack, at);
      stack.set_index(stack.index() + 1);
      return climb_to_separator_after(stack);
    }
    attach(owner, stack, at);
    // At a separator: the first entry of the leftmost leaf right of it.
    stack.set_index(stack.index() + 1);
    stack.template descend_to_leaf<node_end::first>();
    return position_of_stack(stack);
  }

  [[gnu::noinline]] static position cross_backward(const tree<T>* owner, node_stack<T>& stack,
                                                   position at) noexcept {
    if (at.key == nullptr) {
      // From past the last entry: the last.
      if (owner->has_root()) {
        stack.attach(owner->root());
        if (stack.descend_to_last_key()) {
          return position_of_stack(stack);
        }
        stack.detach();
      }
      return {};
    }
    attach(owner, stack, at);
    if (stack.ends_at_leaf()) {
      // Before the leaf's first entry: the separator left of it.
      for (std::size_t level = stack.depth(); level-- > 0;) {
        if (stack.index_at(level) > 0) {
          while (stack.depth() > level) {
            stack.ascend();
          }
          stack.set_index(stack.index() - 1);
          return position_of_stack(stack);
        }
      }
      assert(false && "stepping before the first entry");
      return at;
    }
    // At a separator: the last entry of the rightmost leaf left of it.
    stack.template descend_to_leaf<node_end::last>();
    stack.set_index(stack.index() - 1);
    return position_of_stack(stack);
  }

  // From a node stack at a leaf's last edge: up to the nearest separator
  // right of it, or past the last entry.
  static position climb_to_separator_after(node_stack<T>& stack) noexcept {
    for (std::size_t level = stack.depth(); level-- > 0;) {
      if (stack.index_at(level) < stack.internal_at(level).len()) {
        while (stack.depth() > level) {
          stack.ascend();
        }
        return position_of_stack(stack);
      }
    }
    stack.detach();
    return {};
  }

  const tree<T>* tree_ = nullptr;
  position at_;
  node_stack<T> stack_;
};

}  // namespace bbtree::detail
