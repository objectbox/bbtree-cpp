// Inserting into and removing from a latched tree, as the unsync tree does
// (09-concurrency.md): every thread's changes, run with the unsync tree's own
// operations on the nodes it holds.
//
// Inserts:
// - an equal key stays in place; the insert's policy decides what becomes of
//   its value (replaced, merged, or left and read);
// - an append to the last leaf latches that leaf alone (D45);
// - otherwise a descent latches the leaf exclusively, and the key goes in if
//   the leaf has room; a key that lacks what every key shares first shrinks
//   the shared prefix, holding the root and both borders (D50);
// - a full leaf splits: the insert descends again keeping its path, takes the
//   path from the leaf's parent up to the lowest node with room, and up to
//   the nodes holding that node's fences, then its pages, and runs the unsync
//   split along them.
// Removals:
// - an entry whose leaf keeps its minimum, or is the root, comes out with that
//   leaf latched alone;
// - otherwise the removal descends again keeping its path, and takes, for
//   each level it leaves underfull, the parent and the sibling it merges with
//   or takes from; for a separator, the path below it and the right border of
//   the subtree left of it; and the nodes up to the fences of the topmost
//   changed node. Then it runs the unsync removal along them.
// A failed allocation throws `alloc_error` before anything changes.
#pragma once

#include <bbtree/detail/latched/held.hpp>
#include <bbtree/detail/latched/latched_tree.hpp>

#include <cstddef>
#include <optional>
#include <utility>

namespace bbtree::detail {

template <class T>
class latched_changes {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

 public:
  explicit latched_changes(latched_tree<T>& tree) noexcept : tree_(tree) {}

  // Inserts `key` with `value` unless the tree holds the key; `policy`
  // decides: `policy.occupied(stored_key, stored_value, value)` for an
  // equal key, `policy.vacant(key, value)` right before a new one goes in.
  template <class A, class Policy>
  auto insert(K&& key, V&& value, Policy& policy, const page_source<A>& source) {
    if (!tree_.has_root()) {
      start_tree(source);
    }
    if (auto appended = append(key, value, policy)) {
      return std::move(*appended);
    }
    while (true) {
      auto found = tree_.template descend<leaf_mode::exclusive>(search_key<T, K>(key));
      if (!found) {
        shrink_shared_prefix_for(key);
        continue;
      }
      std::size_t edge = 0;
      if (auto output = insert_into(found->leaf, found->at_separator, key, value, policy, edge)) {
        return std::move(*output);
      }
      found->leaf.release();
      return insert_splitting(key, value, policy, source);
    }
  }

  // Removes the entry at `query` if `condition(key, value)` holds of it.
  template <class Q, class A, class Condition>
  std::optional<std::pair<K, V>> remove(const Q& query, Condition& condition, const page_source<A>& source) {
    if (!tree_.has_root()) {
      return std::nullopt;
    }
    const search_key<T, Q> key(query);
    {
      auto found = tree_.template descend<leaf_mode::exclusive>(key);
      if (!found) {
        return std::nullopt;
      }
      const leaf_node<T> leaf = found->leaf.leaf();
      if (found->at_separator) {
        if (!condition(std::as_const(leaf.head_key()), std::as_const(leaf.head_value()))) {
          return std::nullopt;
        }
      } else {
        const std::optional<std::size_t> index = leaf.find(key);
        if (!index || !condition(leaf.key(*index), std::as_const(leaf.value(*index)))) {
          return std::nullopt;
        }
        if (leaf.keeps_its_minimum() || is_root(leaf)) {
          return leaf.remove(*index);
        }
      }
    }
    node_pages<T, A> pages(tree_.free_, source);
    backoff wait;
    while (true) {
      std::optional<std::pair<K, V>> removed;
      if (try_remove_rebalancing(key, condition, pages, removed)) {
        return removed;
      }
      wait.wait();
    }
  }

 private:
  bool is_root(leaf_node<T> leaf) const noexcept { return tree_.root_node() == leaf.node(); }

  node_ptr<T> rightmost_leaf() const noexcept {
    return node_ptr<T>::from_tagged(tree_.rightmost_leaf_.load(std::memory_order_acquire));
  }
  void point_appends_to(leaf_node<T> leaf) noexcept {
    tree_.rightmost_leaf_.store(leaf.node().tagged(), std::memory_order_release);
  }

  // Starts a tree with an empty root leaf, unless another thread did first.
  template <class A>
  [[gnu::cold, gnu::noinline]] void start_tree(const page_source<A>& source) {
    void* page = source.try_allocate(page_role_traits<T>::kind(page_role::root), T::root_page);
    if (page == nullptr) {
      throw alloc_error();
    }
    const root_page<T> root = root_page<T>::create(fresh_page<T, page_role::root>::adopt(page));
    node_latch latch(root.node().page());
    const latch_version version = latch.lock_exclusive();
    std::uintptr_t expected = 0;
    if (tree_.root_.compare_exchange_strong(expected, root.node().tagged(), std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
      tree_.rightmost_leaf_.store(root.node().tagged(), std::memory_order_release);
      latch.unlock_exclusive(version);
      return;
    }
    std::move(root.free()).deallocate(source);
  }

  // The key, checked to share what every key does: by the descent that found
  // its leaf, or for a root leaf, which holds the shared prefix, by
  // shrinking the prefix first.
  prefixed_key<K&&> prefixed(K& key, leaf_node<T> leaf) {
    if (is_root(leaf)) {
      return tree_.change_held(
          [&](tree<T>& whole) { return whole.template shrink_shared_prefix_for<K&&>(std::move(key)); });
    }
    return prefixed_key<K&&>(std::move(key));
  }

  // An append to the last leaf, if the key goes after its last key and the
  // leaf has room (D45); nothing otherwise.
  template <class Policy>
  auto append(K& key, V& value, Policy& policy) -> std::optional<decltype(policy.vacant(key, value))> {
    const node_ptr<T> last = rightmost_leaf();
    if (!last) {
      return std::nullopt;
    }
    const leaf_node<T> leaf(last);
    held_leaf<T, leaf_mode::exclusive> held(leaf);
    if (rightmost_leaf() != last) {
      return std::nullopt;
    }
    const std::size_t len = leaf.len();
    if (len == 0 || len >= leaf_node<T>::capacity) {
      return std::nullopt;
    }
    if (search_key<T, K>(key).cmp_full(leaf.key(len - 1)) <= 0) {
      return std::nullopt;
    }
    if (!tree_.shared_.template is_shared_by<T>(key)) {
      return std::nullopt;
    }
    auto output = policy.vacant(key, value);
    leaf.insert_fit(len, prefixed_key<K&&>(std::move(key)).template window_at<T>(leaf.prefix_len()),
                    std::move(value));
    return output;
  }

  // Inserts into the held leaf if it has room or the key is there; nothing,
  // with the edge where the key belongs, if the leaf is full.
  template <class Policy>
  auto insert_into(const held_leaf<T, leaf_mode::exclusive>& held, bool at_separator, K& key, V& value,
                   Policy& policy, std::size_t& edge) -> std::optional<decltype(policy.vacant(key, value))> {
    const leaf_node<T> leaf = held.leaf();
    if (at_separator) {
      return policy.occupied(std::as_const(leaf.head_key()), leaf.head_value(), std::move(value));
    }
    const search_result found = leaf.search(search_key<T, K>(key));
    if (found.found) {
      return policy.occupied(leaf.key(found.index), leaf.value(found.index), std::move(value));
    }
    if (leaf.len() < leaf_node<T>::capacity) {
      auto output = policy.vacant(key, value);
      const prefixed_key<K&&> checked = prefixed(key, leaf);
      leaf.insert_fit(found.index, checked.template window_at<T>(leaf.prefix_len()), std::move(value));
      return output;
    }
    edge = found.index;
    return std::nullopt;
  }

  template <class Policy, class A>
  [[gnu::noinline]] auto insert_splitting(K& key, V& value, Policy& policy, const page_source<A>& source)
      -> decltype(policy.vacant(key, value)) {
    node_pages<T, A> pages(tree_.free_, source);
    backoff wait;
    while (true) {
      tree_path<T> path;
      auto found = tree_.template descend<leaf_mode::exclusive>(search_key<T, K>(key), path);
      if (!found) {
        shrink_shared_prefix_for(key);
        continue;
      }
      std::size_t edge = 0;
      if (auto output = insert_into(found->leaf, found->at_separator, key, value, policy, edge)) {
        return std::move(*output);
      }
      if (auto output = split(path, found->leaf, edge, key, value, policy, pages)) {
        return std::move(*output);
      }
      wait.wait();
    }
  }

  // Takes what the split changes and runs it; nothing if another thread
  // holds one of those nodes, to start over.
  template <class Policy, class A>
  auto split(const tree_path<T>& path, const held_leaf<T, leaf_mode::exclusive>& held, std::size_t edge,
             K& key, V& value, Policy& policy, node_pages<T, A>& pages)
      -> std::optional<decltype(policy.vacant(key, value))> {
    const std::size_t depth = path.depth();
    std::optional<std::size_t> room;
    for (std::size_t level = depth; level-- > 0;) {
      if (path.level(level).route.len < internal_node<T>::capacity) {
        room = level;
        break;
      }
    }
    const std::size_t top = path.fences_top(room.value_or(0));
    held_nodes<T> held_internals;
    if (!held_internals.take_path(path, top)) {
      return std::nullopt;
    }
    const std::size_t splits = depth - (room ? *room + 1 : 0);
    const std::size_t leaves = !room && depth == 0 ? 2 : 1;
    const std::size_t internals = room ? splits : (depth == 0 ? 0 : splits + 1);
    if (!pages.reserve(page_kind::leaf, leaves) || !pages.reserve(page_kind::internal, internals)) {
      throw alloc_error();
    }
    const leaf_node<T> leaf = held.leaf();
    auto output = policy.vacant(key, value);
    const bool was_last_leaf = rightmost_leaf() == leaf.node();
    const prefixed_key<K&&> checked = prefixed(key, leaf);
    tree_.change_held([&](tree<T>& whole) {
      node_stack_mut<T> stack(whole, path.stack_to_leaf(top, leaf, edge));
      std::size_t uncounted = 0;
      stack.insert_prefixed(checked, std::move(value), pages, uncounted);
    });
    if (was_last_leaf) {
      const node_ptr<T> took_separator = room ? path.level(*room).node.node() : tree_.root_node();
      point_appends_to(node_end_of<T>::template border_leaf<node_end::last>(took_separator));
    }
    return output;
  }

  // Shrinks what every key shares to what `key` shares too, holding the root
  // and every node along both borders.
  [[gnu::cold, gnu::noinline]] void shrink_shared_prefix_for(K& key) {
    backoff wait;
    while (true) {
      held_nodes<T> held;
      if (take_borders(held)) {
        tree_.change_held([&](tree<T>& whole) { (void)whole.template shrink_shared_prefix_for<K&>(key); });
        return;
      }
      wait.wait();
    }
  }

  bool take_borders(held_nodes<T>& held) noexcept {
    const node_ptr<T> root = tree_.root_node();
    if (!held.take_node(root) || tree_.root_node() != root) {
      return false;
    }
    for (const node_end end : {node_end::first, node_end::last}) {
      node_ptr<T> node = root;
      while (!node.is_leaf()) {
        const internal_node<T> internal(node);
        node = internal.edge(end == node_end::last ? internal.len() : 0);
        if (!held.take_node(node)) {
          return false;
        }
      }
    }
    return true;
  }

  // One try at a removal that rebalances; false to start over.
  template <class Q, class Condition, class A>
  bool try_remove_rebalancing(const search_key<T, Q>& key, Condition& condition, node_pages<T, A>& pages,
                              std::optional<std::pair<K, V>>& removed) {
    tree_path<T> path;
    auto found = tree_.template descend<leaf_mode::exclusive>(key, path);
    if (!found) {
      return true;
    }
    const leaf_node<T> leaf = found->leaf.leaf();
    const std::size_t depth = path.depth();
    std::optional<std::size_t> separator_level;
    std::size_t leaf_index = 0;
    if (found->at_separator) {
      if (!condition(std::as_const(leaf.head_key()), std::as_const(leaf.head_value()))) {
        return true;
      }
      for (std::size_t level = 0; level < depth; ++level) {
        if (path.level(level).route.at_separator) {
          separator_level = level;
          break;
        }
      }
      assert(separator_level && "a descent that matched a separator went through it");
    } else {
      const std::optional<std::size_t> index = leaf.find(key);
      if (!index || !condition(leaf.key(*index), std::as_const(leaf.value(*index)))) {
        return true;
      }
      if (leaf.keeps_its_minimum() || is_root(leaf)) {
        removed = leaf.remove(*index);
        return true;
      }
      leaf_index = *index;
    }
    held_nodes<T> held;
    std::size_t fixes = 0;
    if (!take_fixes(path, leaf.len() - 1, held, fixes)) {
      return false;
    }
    std::size_t top_changed = depth - fixes;
    if (separator_level) {
      const path_level<T>& level = path.level(*separator_level);
      if (!held.take_path(path, *separator_level) ||
          !take_left_border(level.node, level.route.edge - 1, held)) {
        return false;
      }
      top_changed = std::min(top_changed, *separator_level);
    }
    const std::size_t top = path.fences_top(top_changed);
    if (!held.take_path(path, top)) {
      return false;
    }
    removed = tree_.change_held([&](tree<T>& whole) {
      node_stack_mut<T> stack(
          whole, separator_level ? path.stack_to_separator(top, *separator_level,
                                                           path.level(*separator_level).route.edge - 1)
                                 : path.stack_to_leaf(top, leaf, leaf_index));
      std::size_t uncounted = 1;
      return stack.remove(pages, uncounted);
    });
    const node_ptr<T> last = rightmost_leaf();
    if (last && pages.has_freed(last.page())) {
      const node_ptr<T> top_fixed =
          depth - fixes == 0 ? tree_.root_node() : path.level(depth - fixes).node.node();
      point_appends_to(node_end_of<T>::template border_leaf<node_end::last>(top_fixed));
    }
    return true;
  }

  // Takes, from the leaf up, the parent and fixing sibling of every level
  // the removal leaves underfull; `fixes` counts the levels.
  bool take_fixes(const tree_path<T>& path, std::size_t leaf_len, held_nodes<T>& held,
                  std::size_t& fixes) noexcept {
    std::size_t child_len = leaf_len;
    while (true) {
      const std::size_t level = path.depth() - fixes;
      const std::size_t min_len = fixes == 0 ? leaf_node<T>::min_len : internal_node<T>::min_len;
      if (child_len >= min_len || level == 0) {
        return true;
      }
      const path_level<T>& parent = path.level(level - 1);
      if (!held.take_internal_at(parent.node, parent.version)) {
        return false;
      }
      const std::size_t sibling_edge = parent.route.edge > 0 ? parent.route.edge - 1 : parent.route.edge + 1;
      bool merges = false;
      if (fixes == 0) {
        const leaf_node<T> sibling = parent.node.leaf_child(sibling_edge);
        if (!held.take_leaf(sibling)) {
          return false;
        }
        merges = leaf_node<T>::fit_merged(child_len, sibling.len());
      } else {
        const internal_node<T> sibling = parent.node.internal_child(sibling_edge);
        if (!held.take_internal(sibling)) {
          return false;
        }
        merges = internal_node<T>::fit_merged(child_len, sibling.len());
      }
      ++fixes;
      if (!merges) {
        return true;
      }
      child_len = parent.node.len() - 1;
    }
  }

  // Takes the right border of the subtree at `edge` of `node`: the nodes
  // whose upper fence a removed separator's successor becomes.
  bool take_left_border(internal_node<T> node, std::size_t edge, held_nodes<T>& held) noexcept {
    node_ptr<T> child = node.edge(edge);
    while (true) {
      if (child.is_leaf()) {
        return held.take_leaf(leaf_node<T>(child));
      }
      if (!held.take_internal(internal_node<T>(child))) {
        return false;
      }
      child = internal_node<T>(child).edge(child.len());
    }
  }

  latched_tree<T>& tree_;
};

}  // namespace bbtree::detail
