// Node stacks (D11, D24): the nodes from a tree's root down to a position in
// one of them, and the index taken at each. Iterators move two of them through
// the tree, entries keep the one their search found, and inserts and removals
// climb the one that led to the key.
//
// A node stack has a slot per level of the tallest possible tree, the root's
// first. Nothing records how tall a tree is: a node stack counts the levels it
// goes down. Its slots are written only as levels are used, and a node stack
// is never copied once written inside the tree code: a copy's wide loads wait
// for the narrow stores that wrote it (D23).
#pragma once

#include <bbtree/detail/node/internal.hpp>
#include <bbtree/detail/node/root_page.hpp>
#include <bbtree/detail/tree/shared_prefix.hpp>

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace bbtree::detail {

// The deepest a leaf lies below the root: a node below the root has at least
// 5 children, and one level more would need more than 2^64 bytes (D22).
inline constexpr std::size_t max_depth = 23;

// Which end of a node a walk or a border is on: its first edge or its last.
enum class node_end : std::uint8_t { first, last };

// A node's edge at an end, and the leaf at a border under it.
template <class T>
struct node_end_of {
  template <node_end End>
  static std::size_t edge(node_ptr<T> node) noexcept {
    return End == node_end::last ? node.len() : 0;
  }

  template <node_end End>
  static leaf_node<T> border_leaf(node_ptr<T> node) noexcept {
    while (!node.is_leaf()) {
      node = internal_node<T>(node).edge(edge<End>(node));
    }
    return leaf_node<T>(node);
  }
};

// Which side of an equal key a range bound's edge is on.
enum class key_side : std::uint8_t { before, after };

template <class T>
class node_stack {
 public:
  // A detached node stack writes nothing until it is attached.
  node_stack() noexcept {}  // NOLINT: the slots stay unwritten on purpose

  void attach(node_ptr<T> root) noexcept {
    write_level(0, root, 0);
    levels_ = 1;
  }
  void detach() noexcept { levels_ = 0; }

  std::size_t depth() const noexcept { return static_cast<std::size_t>(levels_) - 1; }
  std::size_t levels() const noexcept { return levels_; }
  bool is_at_root() const noexcept { return levels_ == 1; }
  bool is_attached() const noexcept { return levels_ > 0; }

  node_ptr<T> node_at(std::size_t depth) const noexcept {
    assert(depth < levels_ && "the depth is not on the node stack");
    return node_ptr<T>::from_tagged(nodes_[depth]);
  }
  internal_node<T> internal_at(std::size_t depth) const noexcept { return internal_node<T>(node_at(depth)); }
  node_ptr<T> node() const noexcept { return node_at(depth()); }
  bool ends_at_leaf() const noexcept { return node().is_leaf(); }
  leaf_node<T> leaf() const noexcept { return leaf_node<T>(node()); }
  internal_node<T> internal() const noexcept { return internal_node<T>(node()); }

  std::size_t index_at(std::size_t depth) const noexcept {
    assert(depth < levels_);
    return indexes_[depth];
  }
  std::size_t index() const noexcept { return index_at(depth()); }
  void set_index(std::size_t index) noexcept { write_index(depth(), index); }

  // Goes up a level: the node above, internal, at its edge to the node left.
  internal_node<T> ascend() noexcept {
    assert(levels_ > 1 && "ascending from the root");
    --levels_;
    return internal_at(depth());
  }

  // Goes down the bottom node's edge at its index, to that child's edge 0.
  void descend() noexcept {
    const std::size_t at = depth();
    const node_ptr<T> child = internal_at(at).edge(index_at(at));
    write_level(at + 1, child, 0);
    ++levels_;
  }

  // Goes down from the bottom node's edge at its index to a leaf, taking
  // every node's `End` edge on the way.
  template <node_end End>
  void descend_to_leaf() noexcept {
    std::size_t at = depth();
    std::size_t edge = index_at(at);
    node_ptr<T> node = node_at(at);
    while (!node.is_leaf()) {
      node = internal_node<T>(node).edge(edge);
      ++at;
      edge = node_end_of<T>::template edge<End>(node);
      write_level(at, node, edge);
    }
    levels_ = static_cast<std::uint8_t>(at + 1);
  }

  // Goes from the bottom node's `End` edge to its `End` leaf edge.
  template <node_end End>
  void descend_to_end() noexcept {
    set_index(node_end_of<T>::template edge<End>(node()));
    descend_to_leaf<End>();
  }

  // Goes to the first key; false for an empty tree.
  bool descend_to_first_key() noexcept {
    descend_to_end<node_end::first>();
    return node().len() > 0;
  }

  // Goes to the last key; false for an empty tree.
  bool descend_to_last_key() noexcept {
    descend_to_end<node_end::last>();
    if (index() == 0) {
      return false;
    }
    set_index(index() - 1);
    return true;
  }

  // Searches from the root it is attached at for `key`: true at the key, in
  // a leaf or at a separator; false at the leaf edge where it belongs.
  template <class Q>
  bool search(const search_key<T, Q>& key, const shared_prefix& shared) noexcept {
    if (const auto outside = shared.place_outside(key)) {
      if (*outside < 0) {
        descend_to_end<node_end::first>();
      } else {
        descend_to_end<node_end::last>();
      }
      return false;
    }
    std::size_t at = depth();
    node_ptr<T> node = node_at(at);
    while (!node.is_leaf()) {
      const internal_node<T> internal(node);
      const search_result found = internal.search(key);
      write_index(at, found.index);
      if (found.found) {
        levels_ = static_cast<std::uint8_t>(at + 1);
        return true;
      }
      node = internal.edge(found.index);
      ++at;
      nodes_[at] = node.tagged();
    }
    const search_result found = leaf_node<T>(node).search(key);
    write_index(at, found.index);
    levels_ = static_cast<std::uint8_t>(at + 1);
    return found.found;
  }

  // Searches for the leaf edge on `side` of `key`, whether or not it is
  // there: a range bound's position.
  template <class Q>
  void search_to_edge(const search_key<T, Q>& key, key_side side, const shared_prefix& shared) noexcept {
    if (!search(key, shared)) {
      return;
    }
    if (side == key_side::before) {
      descend_to_leaf<node_end::last>();
    } else {
      set_index(index() + 1);
      descend_to_leaf<node_end::first>();
    }
  }

  // Whether both end at the same edge of the same node.
  bool is_at(const node_stack& other) const noexcept {
    return is_attached() && levels_ == other.levels_ && index() == other.index() && node() == other.node();
  }

  // How this leaf edge orders against `other`'s, by their indexes from the
  // root down.
  std::strong_ordering cmp_position(const node_stack& other) const noexcept {
    const std::size_t levels = levels_ < other.levels_ ? levels_ : other.levels_;
    for (std::size_t at = 0; at < levels; ++at) {
      if (indexes_[at] != other.indexes_[at]) {
        return indexes_[at] <=> other.indexes_[at];
      }
    }
    return levels_ <=> other.levels_;
  }

 private:
  void write_level(std::size_t depth, node_ptr<T> node, std::size_t index) noexcept {
    nodes_[depth] = node.tagged();
    write_index(depth, index);
  }
  void write_index(std::size_t depth, std::size_t index) noexcept {
    assert(index <= 0xffff);
    indexes_[depth] = static_cast<std::uint16_t>(index);
  }

  std::uintptr_t nodes_[max_depth + 1];
  std::uint16_t indexes_[max_depth + 1];
  std::uint8_t levels_ = 0;
};

// Where an entry a search found is: key `index` of a leaf, or separator
// `index` of an internal node, whose value is its head's (D17).
template <class T>
struct found_entry {
  node_ptr<T> node;
  std::size_t index;

  bool in_leaf() const noexcept { return node.is_leaf(); }

  typename T::key_type* key() const noexcept {
    if (node.is_leaf()) {
      return leaf_node<T>(node).key_address(index);
    }
    return separator_key(internal_node<T>(node), index);
  }

  typename T::mapped_type* value() const noexcept {
    if (node.is_leaf()) {
      return leaf_node<T>(node).value_address(index);
    }
    return separator_value(internal_node<T>(node), index);
  }

  // Out of line: inlined, the walk to a separator's head sits in every
  // descent's loop (D41).
  [[gnu::noinline]] static typename T::key_type* separator_key(internal_node<T> node,
                                                               std::size_t index) noexcept {
    return node.separator_key_address(index, node.head_leaf(index));
  }
  [[gnu::noinline]] static typename T::mapped_type* separator_value(internal_node<T> node,
                                                                    std::size_t index) noexcept {
    return node.head_leaf(index).head_value_address();
  }
};

}  // namespace bbtree::detail
