// Visiting a latched tree's entries in key order or in reverse, one leaf at a
// time (09-concurrency.md, Scans): the caller's closure runs on each entry
// while its leaf is latched shared, the leaf's head first, then its keys, or
// in reverse its keys from the last, then its head.
//
// - The first leaf is where the bound the scan starts from belongs; without
//   one, or with one before every key (after every key, in reverse), the
//   tree's first leaf, down first edges (its last, down last edges). A bound
//   that lacks what every key shares (D50) has no leaf: which side of every
//   key it is on is read with that border leaf latched, which holds what
//   every key shares still, as a shrink of it takes every border node.
// - The next leaf is the held leaf's neighbor on the scan's side, found from
//   the internal nodes the descent kept: from the nearest one with an edge on
//   that side of the path, down the edges nearest the held leaf, each node
//   read optimistically and checked hand over hand. The scan latches the next
//   leaf before it lets go of the held one, which can't split meanwhile.
// - If a check fails, the scan searches again for the last key it visited,
//   still holding that key's leaf so that the key stays where it is: the
//   leaf's last key going forward, its first in reverse.
//
// No deadlock: a thread waits for a leaf while holding one only here, to
// latch it shared, which waits only for exclusive holders, and those take
// every other node without waiting; so scans in either direction and writers
// form no cycle. A scan is not a snapshot: an entry present for the whole
// scan is visited exactly once.
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

  // Runs `visit(key, value)` on every entry not ordered after `end` (every
  // entry without one), from the last back, until it returns false.
  template <class Q, class Visit>
  void back_from(const Q* end, Visit& visit) {
    if (!tree_.has_root()) {
      return;
    }
    tree_path<T> path;
    held_leaf<T, leaf_mode::shared> held;
    // The held leaf's keys left to visit, from the last: keys 0..remaining.
    std::size_t remaining = 0;
    if (!end_at(end, path, held, remaining)) {
      return;
    }
    while (true) {
      const leaf_node<T> leaf = held.leaf();
      for (; remaining > 0; --remaining) {
        if (!visit(leaf.key(remaining - 1), std::as_const(leaf.value(remaining - 1)))) {
          return;
        }
      }
      if (leaf.has_head()) {
        if (!visit(std::as_const(leaf.head_key()), std::as_const(leaf.head_value()))) {
          return;
        }
      }
      if (!previous_leaf(path, held)) {
        return;
      }
      remaining = held.leaf().len();
    }
  }

 private:
  // Latches the first leaf of a reverse scan, and how many of its keys it
  // visits; its head it always does. False if nothing lies at or before
  // `end`.
  template <class Q>
  bool end_at(const Q* end, tree_path<T>& path, held_leaf<T, leaf_mode::shared>& held,
              std::size_t& remaining) {
    while (end != nullptr) {
      const search_key<T, Q> key(*end);
      if (auto found = tree_.template descend<leaf_mode::shared>(key, path)) {
        held = std::move(found->leaf);
        if (found->at_separator) {
          remaining = 0;
        } else {
          const search_result place = held.leaf().search(key);
          remaining = place.found ? place.index + 1 : place.index;
        }
        return true;
      }
      // The bound lacked what every key shares when the descent read that. A
      // shrink of it since may have put keys after the bound, so the side is
      // read with the last leaf latched (top of the file): the scan starts
      // there if the bound is after every key, visits nothing if it is before
      // them all, and descends again if it has a leaf by now.
      descend_to_border<node_end::last>(path, held);
      const std::optional<std::strong_ordering> outside = tree_.racing_shared().place_outside(key);
      if (outside && *outside > 0) {
        break;
      }
      held.release();
      if (outside) {
        return false;
      }
    }
    if (end == nullptr) {
      descend_to_border<node_end::last>(path, held);
    }
    remaining = held.leaf().len();
    return true;
  }

  // Latches the first leaf of the scan, and the place in it to start from.
  template <class Q>
  bool start_at(const Q* start, tree_path<T>& path, held_leaf<T, leaf_mode::shared>& held,
                std::size_t& position, bool& head_first) {
    while (start != nullptr) {
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
      // The bound lacked what every key shares when the descent read that. A
      // shrink of it since may have put keys before the bound, so the side is
      // read with the first leaf latched (top of the file): the scan starts
      // there if the bound is before every key, visits nothing if it is after
      // them all, and descends again if it has a leaf by now.
      descend_to_border<node_end::first>(path, held);
      const std::optional<std::strong_ordering> outside = tree_.racing_shared().place_outside(key);
      if (outside && *outside < 0) {
        break;
      }
      held.release();
      if (outside) {
        return false;
      }
    }
    if (start == nullptr) {
      descend_to_border<node_end::first>(path, held);
    }
    position = 0;
    head_first = false;
    return true;
  }

  // The tree's first leaf, latched, down first edges; or its last, down last
  // edges.
  template <node_end End>
  void descend_to_border(tree_path<T>& path, held_leaf<T, leaf_mode::shared>& held) {
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
      if (descend_border_edges<End>(internal_node<T>(root), version, path, held)) {
        return;
      }
    }
  }

  // Down first edges from `node`, read at `version`, to a latched leaf, or
  // down last edges, recording the path; false if a check failed.
  template <node_end End>
  bool descend_border_edges(internal_node<T> node, latch_version version, tree_path<T>& path,
                            held_leaf<T, leaf_mode::shared>& held) {
    while (true) {
      const node_latch latch(node.node().page());
      const std::size_t len = reads::header(node.node(), internal_node<T>::capacity).first;
      const std::size_t edge = End == node_end::first ? 0 : len;
      const node_ptr<T> child = reads::child(node, edge);
      if (!latch.is_unchanged(version) || !child) {
        return false;
      }
      path.record(node, version, routed{edge, false, len});
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

  // The nodes a path keeps below level `level`, down to the held leaf, and
  // the versions they were read at: still unchanged after the walk to the
  // neighbor, they still end at the held leaf, which can't split while held,
  // so the leaf found from above is its neighbor (the Rust crate's
  // `next_leaf`; design/09-concurrency.md).
  class kept_levels {
   public:
    kept_levels(const tree_path<T>& path, std::size_t level) noexcept : count_(path.depth() - level) {
      for (std::size_t below = 0; below < count_; ++below) {
        nodes_[below] = path.level(level + below);
      }
    }

    bool unchanged() const noexcept {
      for (std::size_t below = 0; below < count_; ++below) {
        if (!node_latch(nodes_[below].node.node().page()).is_unchanged(nodes_[below].version)) {
          return false;
        }
      }
      return true;
    }

   private:
    std::size_t count_;
    unwritten_array<path_level<T>, max_depth> nodes_;
  };

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
      // The levels below the turn, which still end at the held leaf, are set
      // aside to check; the path is written over in place from the turn on.
      const kept_levels kept(path, level);
      path.truncate(level - 1);
      const std::size_t edge = at.route.edge + 1;
      path.record(at.node, at.version, routed{edge, false, at.route.len});
      held_leaf<T, leaf_mode::shared> next;
      if (descend_from<node_end::first>(at.node, at.version, edge, path, next) && kept.unchanged()) {
        held = std::move(next);
        return true;
      }
      // The path changed: search again for the last key visited, holding it.
      const leaf_node<T> leaf = held.leaf();
      if (!repath(path, leaf.len() > 0 ? leaf.key(leaf.len() - 1) : leaf.head_key())) {
        return false;
      }
    }
  }

  // Latches the held leaf's left neighbor, then lets go of the held one;
  // false before the first leaf.
  bool previous_leaf(tree_path<T>& path, held_leaf<T, leaf_mode::shared>& held) {
    while (true) {
      std::size_t level = path.depth();
      while (level > 0 && path.level(level - 1).route.edge == 0) {
        --level;
      }
      if (level == 0) {
        return false;
      }
      const path_level<T> at = path.level(level - 1);
      // The levels below the turn, which still end at the held leaf, are set
      // aside to check; the path is written over in place from the turn on.
      const kept_levels kept(path, level);
      path.truncate(level - 1);
      const std::size_t edge = at.route.edge - 1;
      path.record(at.node, at.version, routed{edge, false, at.route.len});
      held_leaf<T, leaf_mode::shared> next;
      if (descend_from<node_end::last>(at.node, at.version, edge, path, next) && kept.unchanged()) {
        held = std::move(next);
        return true;
      }
      // The path changed: search again for the first key visited last,
      // holding it. A leaf with a left neighbor has a head.
      const leaf_node<T> leaf = held.leaf();
      if (!repath(path, leaf.has_head() ? leaf.head_key() : leaf.key(0))) {
        return false;
      }
    }
  }

  // From edge `edge` of `node`, read at `version`, down first edges to a
  // latched leaf, or down last edges.
  template <node_end End>
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
    return descend_border_edges<End>(internal_node<T>(child), child_version, path, next);
  }

  // A fresh path to the held leaf, by `held_key`, one of its keys, which
  // can't move while the leaf is held.
  bool repath(tree_path<T>& path, const K& held_key) {
    auto again = tree_.template descend<leaf_mode::optimistic>(search_key<T, K>(held_key), path);
    return again.has_value();
  }

  const latched_tree<T>& tree_;
};

}  // namespace bbtree::detail
