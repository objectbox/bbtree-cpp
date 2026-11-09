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
// Of its tree a cursor keeps the root node, not the tree's address: a tree
// keeps its root page for life (D52), so a cursor stays at its entry when its
// map is swapped or moved, as std::map's iterators do.
//
// A step, its crossing included, is inlined into the loop that steps, and
// nothing it calls takes the cursor's address: the search that
// gives a lookup's cursor its node stack returns the stack by value. So the
// position stays in registers, where an outlined crossing that took the node
// stack by address kept it in memory, a load and a store a step (PORTING.md
// P38). The maps' `for_each` walks with no cursor (`walk.hpp`), as Rust's
// folds do (PORTING.md P16).
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
  explicit cursor(const tree<T>* owner) noexcept : root_(owner->root_or_none()) {}

  // At `entry`, which a lookup or an insert found; its node stack comes on
  // its first crossing.
  cursor(const tree<T>* owner, found_entry<T> entry) noexcept
      : root_(owner->root_or_none()), at_(position_of(entry)) {}

  // At the entry a node stack of `owner` ends at, or past the last if it is
  // detached.
  cursor(const tree<T>* owner, const node_stack<T>& at) noexcept : root_(owner->root_or_none()), stack_(at) {
    if (stack_.is_attached()) {
      at_ = position_of_stack(stack_);
    }
  }

  // At the tree's first entry, or past the last for an empty tree.
  static cursor first(const tree<T>* owner) noexcept { return first_under(owner->root_or_none()); }

  // At the first entry not ordered before `query` (`side` before), or after
  // it (`side` after): a range bound.
  template <class Q>
  static cursor beside(const tree<T>* owner, const Q& query, key_side side) noexcept {
    cursor at(owner);
    if (at.root_) {
      at.stack_.attach(at.root_);
      at.stack_.search_to_edge(search_key<T, Q>(query), side, owner->shared());
      at.at_ = at_leaf_edge(at.stack_);
    }
    return at;
  }

  bool is_end() const noexcept { return at_.key == nullptr; }
  K* key() const noexcept { return at_.key; }
  V* value() const noexcept { return at_.value; }
  friend bool operator==(const cursor& a, const cursor& b) noexcept { return a.at_.key == b.at_.key; }

  // Steps to the next entry, or past the last; inlined with its crossing,
  // so that the position stays in registers (above).
  [[gnu::always_inline]] void next() noexcept {
    assert(!is_end() && "stepping past the end");
    if (at_.key_end != nullptr) {
      at_.key = advance(at_.key, T::leaf.keys.key_stride);
      at_.value = advance(at_.value, T::leaf.value_stride);
      if (at_.key != at_.key_end) {
        return;
      }
    }
    at_ = cross_forward(root_, stack_, at_);
  }

  // Steps to the previous entry; from past the last, to the last. Inlined
  // with its crossing, as `next` (above).
  [[gnu::always_inline]] void prev() noexcept {
    prev_or_end();
    assert(!is_end() && "stepping before the first entry");
  }

  // As `prev`, and from the first entry to past the last: a reverse
  // iterator's step, whose end is that position (`reverse_iterator.hpp`).
  [[gnu::always_inline]] void prev_or_end() noexcept {
    if (at_.key_end != nullptr && at_.key != key_begin(at_)) {
      at_.key = advance(at_.key, -static_cast<std::ptrdiff_t>(T::leaf.keys.key_stride));
      at_.value = advance(at_.value, -static_cast<std::ptrdiff_t>(T::leaf.value_stride));
      return;
    }
    at_ = cross_backward(root_, stack_, at_);
  }

  // As `next`, and from past the last entry to the first: a reverse
  // iterator's step back from its end.
  void next_or_first() noexcept {
    if (is_end()) {
      *this = first_under(root_);
    } else {
      next();
    }
  }

  // The node stack at the entry, attached first if the cursor has none; for
  // a removal at the cursor.
  const node_stack<T>& attached_stack() noexcept {
    attach(root_, stack_, at_);
    return stack_;
  }

 private:
  // At the first entry of the tree under `root`, or past the last.
  static cursor first_under(node_ptr<T> root) noexcept {
    cursor at;
    at.root_ = root;
    if (root) {
      at.stack_.attach(root);
      if (at.stack_.descend_to_first_key()) {
        at.at_ = position_of_stack(at.stack_);
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
  [[gnu::always_inline]] static void attach(node_ptr<T> root, node_stack<T>& stack,
                                            const position& at) noexcept {
    if (stack.is_attached()) {
      if (at.key_end != nullptr) {
        const auto from_end = static_cast<std::size_t>(reinterpret_cast<std::byte*>(at.key_end) -
                                                       reinterpret_cast<std::byte*>(at.key)) /
                              T::leaf.keys.key_stride;
        stack.set_index(at.leaf_len - from_end);
      }
      return;
    }
    // Returned by value, not filled in through the cursor's address, so that
    // the cursor's position can stay in registers.
    stack = searched_stack(root, at.key);
  }

  // The node stack at the entry whose key is at `key`: where a search for
  // the key ends. A key whose order or encoding lies (PORTING.md, P13) may
  // be found elsewhere or nowhere; its entry is then found by its address,
  // so that what steps or erases from the stack is at an entry all the same.
  [[gnu::noinline]] static node_stack<T> searched_stack(node_ptr<T> root, const K* key) noexcept {
    node_stack<T> stack;
    stack.attach(root);
    if (stack.search_sharing_prefix(search_key<T, K>(*key)) &&
        found_entry<T>{stack.node(), stack.index()}.key() == key) [[likely]] {
      return stack;
    }
    cursor walker = first_under(root);
    while (!walker.is_end() && walker.key() != key) {
      walker.next();
    }
    assert(!walker.is_end() && "a cursor's key is in its tree");
    return walker.attached_stack();
  }

  [[gnu::always_inline]] static position cross_forward(node_ptr<T> root, node_stack<T>& stack,
                                                       position at) noexcept {
    if (at.key_end != nullptr) {
      // Past the leaf's last entry: the leaf edge after it.
      at.key = advance(at.key, -static_cast<std::ptrdiff_t>(T::leaf.keys.key_stride));
      attach(root, stack, at);
      stack.set_index(stack.index() + 1);
      return climb_to_separator_after(stack);
    }
    attach(root, stack, at);
    // At a separator: the first entry of the leftmost leaf right of it.
    stack.set_index(stack.index() + 1);
    stack.template descend_to_leaf<node_end::first>();
    return position_of_stack(stack);
  }

  [[gnu::always_inline]] static position cross_backward(node_ptr<T> root, node_stack<T>& stack,
                                                        position at) noexcept {
    if (at.key == nullptr) {
      // From past the last entry: the last.
      if (root) {
        stack.attach(root);
        if (stack.descend_to_last_key()) {
          return position_of_stack(stack);
        }
        stack.detach();
      }
      return {};
    }
    attach(root, stack, at);
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
      // Nothing is before the first entry.
      stack.detach();
      return {};
    }
    // At a separator: the last entry of the rightmost leaf left of it.
    stack.template descend_to_leaf<node_end::last>();
    stack.set_index(stack.index() - 1);
    return position_of_stack(stack);
  }

  // From a node stack at a leaf's last edge: up to the nearest separator
  // right of it, or past the last entry.
  [[gnu::always_inline]] static position climb_to_separator_after(node_stack<T>& stack) noexcept {
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

  // The root node of the cursor's tree; none for a tree without a root page.
  node_ptr<T> root_;
  position at_;
  node_stack<T> stack_;
};

}  // namespace bbtree::detail
