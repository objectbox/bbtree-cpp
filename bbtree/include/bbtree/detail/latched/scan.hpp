// Visiting a latched tree's entries in key order, one leaf at a time
// (09-concurrency.md, Scans): the caller's closure runs on each entry while
// its leaf is latched shared, the leaf's head first, then its keys.
//
// - The first leaf is where the start bound belongs; without one, or with one
//   before every key, the tree's first leaf.
// - The next leaf is found from the internal nodes the descent kept: from the
//   nearest one with an edge right of the path, down first edges, each node
//   read optimistically and checked hand over hand. The scan latches the next
//   leaf before it lets go of the held one, which can't split meanwhile.
// - If a check fails, the scan searches again for the last key it visited,
//   still holding that key's leaf so that the key stays where it is.
//
// No deadlock: a thread waits for a leaf while holding one only here, to
// latch it shared, which waits only for exclusive holders, and those take
// every other node without waiting. A scan is not a snapshot: an entry
// present for the whole scan is visited exactly once.
#pragma once

#include <bbtree/detail/latched/held.hpp>
#include <bbtree/detail/latched/latched_tree.hpp>

#include <cstddef>
#include <optional>

namespace bbtree::detail {

template <class T>
class latched_scan {
  using K = typename T::key_type;
  using V = typename T::mapped_type;
  using reads = racing_node_reads<T>;

 public:
  explicit latched_scan(const latched_tree<T>& tree) noexcept : tree_(tree) {}

  // Runs `visit(key, value)` on every entry from `start` on (from the first
  // without one), until it returns false.
  template <class Q, class Visit>
  void from(const Q* start, Visit& visit) {
    if (!tree_.has_root()) {
      return;
    }
    tree_path<T> path;
    held_leaf<T, leaf_mode::shared> held;
    std::size_t position = 0;
    bool head_first = false;
    if (!start_at(start, path, held, position, head_first)) {
      return;
    }
    while (true) {
      const leaf_node<T> leaf = held.leaf();
      if (head_first && leaf.has_head()) {
        if (!visit(std::as_const(leaf.head_key()), std::as_const(leaf.head_value()))) {
          return;
        }
      }
      for (; position < leaf.len(); ++position) {
        if (!visit(leaf.key(position), std::as_const(leaf.value(position)))) {
          return;
        }
      }
      if (!next_leaf(path, held)) {
        return;
      }
      position = 0;
      head_first = true;
    }
  }

 private:
  // Latches the first leaf of the scan, and the place in it to start from.
  template <class Q>
  bool start_at(const Q* start, tree_path<T>& path, held_leaf<T, leaf_mode::shared>& held,
                std::size_t& position, bool& head_first) {
    if (start != nullptr) {
      const search_key<T, Q> key(*start);
      if (auto found = tree_.template descend<leaf_mode::shared>(key, path)) {
        held = std::move(found->leaf);
        if (found->at_separator) {
          position = 0;
          head_first = true;
        } else {
          position = held.leaf().search(key).index;
          head_first = false;
        }
        return true;
      }
      // Outside what every key shares: before every key, or after.
      const std::optional<std::strong_ordering> outside = tree_.racing_shared().place_outside(key);
      if (outside && *outside > 0) {
        return false;
      }
    }
    descend_to_first(path, held);
    position = 0;
    head_first = false;
    return true;
  }

  // The tree's first leaf, latched, down first edges.
  void descend_to_first(tree_path<T>& path, held_leaf<T, leaf_mode::shared>& held) {
    while (true) {
      path.clear();
      const node_ptr<T> root = tree_.root_node();
      if (root.is_leaf()) {
        held = held_leaf<T, leaf_mode::shared>(leaf_node<T>(root));
        if (tree_.root_node() == root) {
          return;
        }
        continue;
      }
      latch_version version = node_latch(root.page()).read_version();
      if (tree_.root_node() != root) {
        continue;
      }
      if (descend_first_edges(internal_node<T>(root), version, path, held)) {
        return;
      }
    }
  }

  // Down first edges from `node`, read at `version`, to a latched leaf,
  // recording the path; false if a check failed.
  bool descend_first_edges(internal_node<T> node, latch_version version, tree_path<T>& path,
                           held_leaf<T, leaf_mode::shared>& held) {
    while (true) {
      const node_latch latch(node.node().page());
      const std::size_t len = reads::header(node.node(), internal_node<T>::capacity).first;
      const node_ptr<T> child = reads::child(node, 0);
      if (!latch.is_unchanged(version) || !child) {
        return false;
      }
      path.record(node, version, routed{0, false, len});
      if (child.is_leaf()) {
        held_leaf<T, leaf_mode::shared> next{leaf_node<T>(child)};
        if (!latch.is_unchanged(version)) {
          return false;
        }
        held = std::move(next);
        return true;
      }
      const latch_version child_version = node_latch(child.page()).read_version();
      if (!latch.is_unchanged(version)) {
        return false;
      }
      node = internal_node<T>(child);
      version = child_version;
    }
  }

  // Latches the held leaf's right neighbor, then lets go of the held one;
  // false past the last leaf.
  bool next_leaf(tree_path<T>& path, held_leaf<T, leaf_mode::shared>& held) {
    while (true) {
      std::size_t level = path.depth();
      while (level > 0 && path.level(level - 1).route.edge >= path.level(level - 1).route.len) {
        --level;
      }
      if (level == 0) {
        return false;
      }
      const path_level<T> at = path.level(level - 1);
      tree_path<T> next_path;
      for (std::size_t above = 0; above + 1 < level; ++above) {
        next_path.record(path.level(above).node, path.level(above).version, path.level(above).route);
      }
      const std::size_t edge = at.route.edge + 1;
      next_path.record(at.node, at.version, routed{edge, false, at.route.len});
      held_leaf<T, leaf_mode::shared> next;
      if (descend_from(at.node, at.version, edge, next_path, next)) {
        held = std::move(next);
        path = next_path;
        return true;
      }
      // The path changed: search again for the last key visited, holding it.
      if (!repath(path, held)) {
        return false;
      }
    }
  }

  // From edge `edge` of `node`, read at `version`, down first edges to a
  // latched leaf.
  bool descend_from(internal_node<T> node, latch_version version, std::size_t edge, tree_path<T>& path,
                    held_leaf<T, leaf_mode::shared>& next) {
    const node_latch latch(node.node().page());
    const node_ptr<T> child = reads::child(node, edge);
    if (!latch.is_unchanged(version) || !child) {
      return false;
    }
    if (child.is_leaf()) {
      held_leaf<T, leaf_mode::shared> leaf{leaf_node<T>(child)};
      if (!latch.is_unchanged(version)) {
        return false;
      }
      next = std::move(leaf);
      return true;
    }
    const latch_version child_version = node_latch(child.page()).read_version();
    if (!latch.is_unchanged(version)) {
      return false;
    }
    return descend_first_edges(internal_node<T>(child), child_version, path, next);
  }

  // A fresh path to the held leaf, by its last key, which can't move while
  // the leaf is held.
  bool repath(tree_path<T>& path, const held_leaf<T, leaf_mode::shared>& held) {
    const leaf_node<T> leaf = held.leaf();
    const K& last = leaf.len() > 0 ? leaf.key(leaf.len() - 1) : leaf.head_key();
    auto again = tree_.template descend<leaf_mode::optimistic>(search_key<T, K>(last), path);
    return again.has_value();
  }

  const latched_tree<T>& tree_;
};

}  // namespace bbtree::detail
