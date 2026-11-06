// What an operation that changes more than its leaf holds, and the path its
// descent kept (09-concurrency.md, "Changes to more than a leaf").
//
// Before it changes any node, an operation takes every node it will change or
// read a separator of (`held_nodes`), all without waiting: internal nodes by
// a compare-and-swap at the version it read them at, or at the version they
// are at; leaves by an exclusive latch tried once. It holds its own leaf
// already and waits for nothing while it does, so no two operations wait on
// each other. A node another thread holds, or one that changed, makes it let
// go of all and start over.
//
// Holding a node holds its separators' keys too, though they are heads of
// leaves below (D49): only a thread holding the node changes them.
#pragma once

#include <bbtree/detail/latched/optimistic.hpp>
#include <bbtree/detail/tree/node_stack.hpp>

#include <cassert>
#include <cstddef>

namespace bbtree::detail {

// One level of a descent: the internal node, the version it was read at, and
// where routing went.
template <class T>
struct path_level {
  internal_node<T> node;
  latch_version version;
  routed route;
};

// The internal nodes a descent went through, root first.
template <class T>
class tree_path {
 public:
  tree_path() noexcept {}  // NOLINT: levels stay unwritten until recorded

  void clear() noexcept { depth_ = 0; }
  void record(internal_node<T> node, latch_version version, routed route) noexcept {
    levels_[depth_++] = {node, version, route};
  }
  std::size_t depth() const noexcept { return depth_; }
  const path_level<T>& level(std::size_t at) const noexcept {
    assert(at < depth_);
    return levels_[at];
  }

  // The topmost level an operation that changes `level` must hold, for the
  // changed nodes' prefixes to fit their fences: the nearest levels above it
  // with a separator on each side of the path; a side without one is the
  // tree's border.
  std::size_t fences_top(std::size_t at) const noexcept {
    std::size_t top = at;
    bool low = false;
    bool high = false;
    for (std::size_t above = at; above-- > 0 && !(low && high);) {
      const routed& route = levels_[above].route;
      if (!low && route.edge > 0) {
        low = true;
        top = above;
      }
      if (!high && route.edge < route.len) {
        high = true;
        top = above;
      }
    }
    return top;
  }

  // A node stack from level `top` down the path to `leaf`, at `index`.
  node_stack<T> stack_to_leaf(std::size_t top, leaf_node<T> leaf, std::size_t index) const noexcept {
    node_stack<T> stack;
    for (std::size_t at = top; at < depth_; ++at) {
      stack.push_level(levels_[at].node.node(), levels_[at].route.edge);
    }
    stack.push_level(leaf.node(), index);
    return stack;
  }

  // A node stack from level `top` down to the separator at `index` of the
  // node at `level`.
  node_stack<T> stack_to_separator(std::size_t top, std::size_t level, std::size_t index) const noexcept {
    node_stack<T> stack;
    for (std::size_t at = top; at < level; ++at) {
      stack.push_level(levels_[at].node.node(), levels_[at].route.edge);
    }
    stack.push_level(levels_[level].node.node(), index);
    return stack;
  }

 private:
  path_level<T> levels_[max_depth];
  std::size_t depth_ = 0;
};

// A descent that keeps no path.
struct no_path {
  void clear() noexcept {}
  template <class... Args>
  void record(Args&&...) noexcept {}
};

template <class T>
class held_nodes {
 public:
  held_nodes() noexcept = default;
  held_nodes(const held_nodes&) = delete;
  held_nodes& operator=(const held_nodes&) = delete;
  ~held_nodes() {
    for (std::size_t at = 0; at < internal_count_; ++at) {
      node_latch(internals_[at].node.page()).unlock_exclusive(internals_[at].version);
    }
    for (std::size_t at = 0; at < leaf_count_; ++at) {
      node_latch(leaves_[at].node.page()).unlock_exclusive(leaves_[at].version);
    }
  }

  bool take_internal_at(internal_node<T> node, latch_version version) noexcept {
    if (holds(internals_, internal_count_, node.node())) {
      return true;
    }
    if (!node_latch(node.node().page()).try_lock_at(version)) {
      return false;
    }
    assert(internal_count_ < max_internals);
    internals_[internal_count_++] = {node.node(), version};
    return true;
  }

  bool take_internal(internal_node<T> node) noexcept {
    if (holds(internals_, internal_count_, node.node())) {
      return true;
    }
    latch_version version;
    return node_latch(node.node().page()).try_read_version(version) && take_internal_at(node, version);
  }

  bool take_leaf(leaf_node<T> leaf) noexcept {
    if (holds(leaves_, leaf_count_, leaf.node())) {
      return true;
    }
    latch_version version;
    if (!node_latch(leaf.node().page()).try_lock_exclusive(version)) {
      return false;
    }
    assert(leaf_count_ < max_leaves);
    leaves_[leaf_count_++] = {leaf.node(), version};
    return true;
  }

  bool take_node(node_ptr<T> node) noexcept {
    return node.is_leaf() ? take_leaf(leaf_node<T>(node)) : take_internal(internal_node<T>(node));
  }

  // The path's nodes from level `top` down, at the versions the descent read.
  bool take_path(const tree_path<T>& path, std::size_t top) noexcept {
    for (std::size_t at = top; at < path.depth(); ++at) {
      if (!take_internal_at(path.level(at).node, path.level(at).version)) {
        return false;
      }
    }
    return true;
  }

 private:
  static constexpr std::size_t max_internals = 3 * max_depth + 2;
  static constexpr std::size_t max_leaves = 4;
  struct held {
    node_ptr<T> node;
    latch_version version;
  };
  static bool holds(const held* nodes, std::size_t count, node_ptr<T> node) noexcept {
    for (std::size_t at = 0; at < count; ++at) {
      if (nodes[at].node == node) {
        return true;
      }
    }
    return false;
  }

  held internals_[max_internals];
  std::size_t internal_count_ = 0;
  held leaves_[max_leaves];
  std::size_t leaf_count_ = 0;
};

}  // namespace bbtree::detail
