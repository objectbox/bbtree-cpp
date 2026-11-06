// Internal nodes: the nodes that route (D12, D17, D32, D49). An internal
// node's page holds its children, one more than its separators, and no values.
// With inline keys it holds only its separators' inline keys and its edges,
// in a 512-byte page by default; each separator's key is the head key of the
// leftmost leaf right of it, which a search reads only on a tie. Without
// inline keys the node keeps its separator keys.
#pragma once

#include <bbtree/detail/node/leaf.hpp>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>

namespace bbtree::detail {

template <class T>
class internal_node : public child_node<T, internal_node<T>, false> {
  using K = typename T::key_type;
  using V = typename T::mapped_type;
  using base = child_node<T, internal_node<T>, false>;

 public:
  using columns_type = typename base::columns_type;
  static constexpr internal_layout layout = T::internal;
  static constexpr std::size_t capacity = base::capacity;
  using page_type = fresh_page<T, page_role::internal>;

  internal_node() noexcept = default;
  explicit internal_node(node_ptr<T> node) noexcept : node_(node) {
    assert(!node.is_leaf() && "the node is a leaf");
  }

  // A new internal node with `first_child` as its only child, at `prefix_len`.
  static internal_node create(page_type&& page, node_ptr<T> first_child, std::size_t prefix_len) noexcept {
    const internal_node node(node_ptr<T>::template initialize<false>(std::move(page).release()));
    node.put_only_child(first_child);
    node.node_.set_prefix_len(prefix_len);
    return node;
  }

  page_type free() const noexcept { return page_type::adopt(node_.page()); }

  node_ptr<T> node() const noexcept { return node_; }
  std::size_t len() const noexcept { return node_.len(); }
  std::size_t prefix_len() const noexcept { return node_.prefix_len(); }
  friend bool operator==(internal_node, internal_node) = default;

  void put_only_child(node_ptr<T> child) const noexcept {
    assert(len() == 0 && "a node with separators has children");
    edge_slots().construct(0, node_ptr<T>(child));
  }

  // --- Children ---

  node_ptr<T> edge(std::size_t index) const noexcept {
    assert(index <= len() && "edge index out of bounds");
    return *edge_slots().slot(index);
  }
  bool has_leaf_children() const noexcept { return edge(0).is_leaf(); }
  leaf_node<T> leaf_child(std::size_t index) const noexcept { return leaf_node<T>(edge(index)); }
  internal_node internal_child(std::size_t index) const noexcept { return internal_node(edge(index)); }

  // The leftmost leaf right of separator `index`, which holds its value as
  // its head, and with inline keys its key (D17, D49).
  leaf_node<T> head_leaf(std::size_t index) const noexcept {
    node_ptr<T> node = edge(index + 1);
    while (!node.is_leaf()) {
      node = internal_node(node).edge(0);
    }
    return leaf_node<T>(node);
  }

  // --- Separators ---

  // Separator `index`'s key: in a head below with inline keys, in this node
  // without.
  const K& separator_key(std::size_t index) const noexcept {
    assert(index < len() && "separator index out of bounds");
    if constexpr (T::has_inline_keys) {
      return head_leaf(index).head_key();
    } else {
      return *key_slots_only().slot(index);
    }
  }
  const K& key(std::size_t index) const noexcept { return separator_key(index); }

  // Where separator `index`'s key is, given its head leaf.
  K* separator_key_address(std::size_t index, leaf_node<T> head) const noexcept {
    if constexpr (T::has_inline_keys) {
      (void)index;
      return head.head_key_slot().slot(0);
    } else {
      (void)head;
      return key_slots_only().slot(index);
    }
  }

  auto inline_keys() const noexcept
    requires columns_type::has_inline_keys
  {
    return columns_type(node_.page()).inline_keys(len());
  }

  // Separator `index`'s window at this node's prefix length, all eight
  // bytes: read again from the key where the node keeps fewer (D43).
  window window_at(std::size_t index) const noexcept {
    assert(index < len());
    if constexpr (!columns_type::has_inline_keys) {
      return {};
    } else if constexpr (base::plan.rehydrates_windows) {
      const std::size_t prefix = prefix_len();
      return {prefix,
              while_keys_move::call([&] { return T::inliner::inline_key_at(separator_key(index), prefix); })};
    } else {
      return {prefix_len(), columns_type(node_.page()).read_inline_key(index)};
    }
  }

  // The window of a separator beside edge `index`, a fence of that child at
  // this node's prefix length; none for an only child.
  std::optional<window> fence_window(std::size_t index) const noexcept {
    assert(index <= len());
    if (index > 0) {
      return window_at(index - 1);
    }
    if (index < len()) {
      return window_at(index);
    }
    return std::nullopt;
  }

  separator<T> replace_separator(std::size_t index, separator<T>&& with) const noexcept {
    assert(index < len());
    separator<T> old = read_separator(index);
    write_separator(index, std::move(with));
    return old;
  }

  // Replaces separator `index` with one from a child, at its prefix length.
  separator<T> replace_separator_from_below(std::size_t index, separator<T>&& with) const noexcept {
    return replace_separator(index, std::move(with).moved_back_to(window_at(index)));
  }

  // Replaces separator `index`'s key, wherever it is kept, and returns it.
  K replace_key(std::size_t index, windowed_key<K>&& key) const noexcept {
    if constexpr (T::has_inline_keys) {
      const window kept = key.window;
      K old = head_leaf(index).replace_head_key(std::move(key.key));
      replace_separator_from_below(index, separator<T>(kept));
      return old;
    } else {
      return std::move(replace_separator_from_below(index, separator<T>(std::move(key))).take_key().key);
    }
  }

  // --- Searching ---

  template <class Q>
  [[gnu::always_inline]] search_result search(const search_key<T, Q>& key) const noexcept {
    if constexpr (columns_type::has_inline_keys && search_key<T, Q>::compares_inline) {
      const window target = key.inline_target(prefix_len());
      const slot_range candidates = inline_keys().equal_range(target.inline_key());
      if (candidates.empty()) {
        return {candidates.start, false};
      }
      return search_candidates(candidates, key, target);
    } else if constexpr (T::has_inline_keys) {
      return search_list(head_keys(*this, 0, len()), key);
    } else {
      return search_list(node_keys<K>(key_slots_only(), 0, len()), key);
    }
  }

  // --- Changing separators and edges ---

  // Inserts `middle` at `index` and `right_child` at edge `index + 1`; room.
  void insert_fit(std::size_t index, separator<T>&& middle, node_ptr<T> right_child) const noexcept {
    const std::size_t old_len = len();
    assert(index <= old_len && old_len < capacity);
    separator<T> fitted = separator_from_child(index, std::move(middle));
    insert_separator(index, old_len, std::move(fitted));
    edge_slots().insert(index + 1, old_len + 1, node_ptr<T>(right_child));
    node_.set_len(old_len + 1);
  }

  split_result<T, internal_node> split_insert(std::size_t index, separator<T>&& middle,
                                              node_ptr<T> right_child,
                                              page_type&& right_page) const noexcept {
    assert(len() == capacity);
    const auto [separator_index, inserted] = insertion_side::of_split(capacity, index, base::min_len);
    const internal_node right(node_ptr<T>::template initialize<false>(std::move(right_page).release()));
    right.node_.set_prefix_len(prefix_len());
    separator<T> up = split_around(separator_index, right);
    (inserted.right ? right : *this).insert_fit(inserted.index, std::move(middle), right_child);
    return {std::move(up), right, inserted};
  }

  // Removes separator `index` and the edge right of it.
  std::pair<separator<T>, node_ptr<T>> remove_with_right_child(std::size_t index) const noexcept {
    const std::size_t old_len = len();
    assert(index < old_len);
    separator<T> removed = remove_separator(index, old_len);
    const node_ptr<T> child = edge_slots().remove(index + 1, old_len + 1);
    set_len_refilling(old_len - 1);
    return {std::move(removed), child};
  }

  // Cuts the node at edge `index`: separators from `index` on and the edges
  // after edge `index` move into a new node on `page`, after `first_child`.
  internal_node cut_off_at(std::size_t index, node_ptr<T> first_child, page_type&& page) const noexcept {
    const std::size_t old_len = len();
    assert(index <= old_len);
    const std::size_t count = old_len - index;
    const internal_node right = create(std::move(page), first_child, prefix_len());
    move_separators(index, count, right, 0);
    edge_slots().move_run_to(index + 1, count, right.edge_slots(), 1);
    right.set_len_refilling(count);
    set_len_refilling(index);
    return right;
  }

  // As far as `prefix_len`, but no further than its children's prefixes.
  std::size_t allowed_prefix_len(std::size_t wanted) const noexcept {
    if (wanted <= prefix_len()) {
      return wanted;
    }
    for (std::size_t index = 0; index <= len(); ++index) {
      wanted = std::min(wanted, edge(index).prefix_len());
    }
    return wanted;
  }

  // --- Balancing children (`siblings.hpp`) ---

  static void steal_left(internal_node parent, std::size_t index, internal_node left, internal_node right,
                         window fill) noexcept {
    const std::size_t left_len = left.len();
    const std::size_t right_len = right.len();
    separator<T> up = left.remove_separator(left_len - 1, left_len);
    separator<T> down = parent.replace_separator(index, std::move(up).moved_back_to(fill));
    right.insert_separator(0, right_len, std::move(down));
    const node_ptr<T> edge = left.edge_slots().remove(left_len, left_len + 1);
    right.edge_slots().insert(0, right_len + 1, node_ptr<T>(edge));
    left.set_len_refilling(left_len - 1);
    right.node_.set_len(right_len + 1);
  }

  static void steal_right(internal_node parent, std::size_t index, internal_node left, internal_node right,
                          window fill) noexcept {
    const std::size_t left_len = left.len();
    const std::size_t right_len = right.len();
    separator<T> up = right.remove_separator(0, right_len);
    separator<T> down = parent.replace_separator(index, std::move(up).moved_back_to(fill));
    left.insert_separator(left_len, left_len, std::move(down));
    const node_ptr<T> edge = right.edge_slots().remove(0, right_len + 1);
    left.edge_slots().insert(left_len + 1, left_len + 1, node_ptr<T>(edge));
    left.node_.set_len(left_len + 1);
    right.set_len_refilling(right_len - 1);
  }

  static void append(internal_node left, window /*fill*/, separator<T>&& middle,
                     internal_node right) noexcept {
    const std::size_t left_len = left.len();
    const std::size_t right_len = right.len();
    left.write_separator(left_len, std::move(middle));
    right.move_separators(0, right_len, left, left_len + 1);
    right.edge_slots().move_run_to(0, right_len + 1, left.edge_slots(), left_len + 1);
    left.node_.set_len(left_len + 1 + right_len);
  }

  internal_node move_to(std::byte* dest) const noexcept {
    if constexpr (T::has_inline_keys || slots<K>::moves_as_bytes) {
      page_copy<T>::keeping_latch(dest, node_.page(), layout.page.size);
      return internal_node(node_ptr<T>::internal_at(dest));
    } else {
      const internal_node moved(node_ptr<T>::template initialize<false>(dest));
      const std::size_t count = len();
      move_separators(0, count, moved, 0);
      edge_slots().move_run_to(0, count + 1, moved.edge_slots(), 0);
      moved.node_.set_len(count);
      return moved;
    }
  }

  void set_len_refilling(std::size_t new_len) const noexcept {
    assert(new_len <= capacity);
    columns_type(node_.page()).mark_vacant(new_len, len());
    node_.set_len(new_len);
  }

  slots<node_ptr<T>> edge_slots() const noexcept {
    return {node_.page() + layout.edges_offset, layout.edge_stride};
  }

  // The separator keys this node keeps, without inline keys.
  slots<K> separator_key_slots() const noexcept
    requires(!T::has_inline_keys)
  {
    return key_slots_only();
  }

 private:
  friend class leaf_node<T>;

  // An internal node's separators' head keys, as a search reads them (D49).
  class head_keys {
   public:
    head_keys(internal_node node, std::size_t start, std::size_t size) noexcept
        : node_(node), start_(start), size_(size) {}
    std::size_t size() const noexcept { return size_; }
    const K& get(std::size_t index) const noexcept { return node_.head_leaf(start_ + index).head_key(); }

   private:
    internal_node node_;
    std::size_t start_;
    std::size_t size_;
  };

  slots<K> key_slots_only() const noexcept {
    return {node_.page() + layout.keys.keys_offset, layout.keys.key_stride};
  }
  key_slots<T, false> keys_and_columns() const noexcept { return {node_.page(), prefix_len()}; }

  template <class List, class Q>
  static search_result search_list(const List& keys, const search_key<T, Q>& key) noexcept {
    if constexpr (base::plan.all_keys_search == key_search::branchless) {
      return key_list_search::branchless<T>(keys, key.key());
    } else {
      return key_list_search::early_exit(keys,
                                         [&](const K& probe) { return T::order::cmp(key.key(), probe); });
    }
  }

  template <class Q>
  search_result search_candidates(slot_range candidates, const search_key<T, Q>& key,
                                  window target) const noexcept {
    const head_keys keys(*this, candidates.start, candidates.size());
    search_result found;
    if constexpr (base::plan.candidate_search == key_search::branchless) {
      found = key_list_search::branchless<T>(keys, key.key());
    } else {
      found = key_list_search::early_exit(
          keys, [&](const K& stored) { return key.cmp_tied(*base::plan.width, target, stored); });
    }
    found.index += candidates.start;
    return found;
  }

  // A separator from a child, at the child's prefix, moved back to this
  // node's prefix, filled from a fence beside edge `index`.
  separator<T> separator_from_child(std::size_t index, separator<T>&& from) const noexcept {
    if (const std::optional<window> fill = fence_window(index)) {
      return std::move(from).moved_back_to(*fill);
    }
    assert(from.window().prefix_len() == prefix_len() || !T::has_inline_keys);
    return std::move(from);
  }

  // The separator slots: keys with their columns without inline keys, the
  // columns alone with them.
  separator<T> read_separator(std::size_t index) const noexcept {
    if constexpr (T::has_inline_keys) {
      return separator<T>(window(prefix_len(), columns_type(node_.page()).read_inline_key(index)));
    } else {
      return separator<T>(keys_and_columns().read(index));
    }
  }
  void write_separator(std::size_t index, separator<T>&& with) const noexcept {
    if constexpr (T::has_inline_keys) {
      assert(with.window().prefix_len() == prefix_len());
      columns_type(node_.page()).write(index, with.window().inline_key(), 0);
    } else {
      keys_and_columns().write(index, std::move(with).take_key());
    }
  }
  void insert_separator(std::size_t index, std::size_t old_len, separator<T>&& with) const noexcept {
    if constexpr (T::has_inline_keys) {
      assert(with.window().prefix_len() == prefix_len());
      columns_type(node_.page()).insert(index, old_len, with.window().inline_key(), 0);
    } else {
      keys_and_columns().insert(index, old_len, std::move(with).take_key());
    }
  }
  separator<T> remove_separator(std::size_t index, std::size_t old_len) const noexcept {
    if constexpr (T::has_inline_keys) {
      const window removed = window_kept(index);
      columns_type(node_.page()).remove(index, old_len);
      return separator<T>(removed);
    } else {
      return separator<T>(keys_and_columns().remove(index, old_len));
    }
  }
  void move_separators(std::size_t start, std::size_t count, internal_node dest,
                       std::size_t dest_start) const noexcept {
    if constexpr (T::has_inline_keys) {
      assert(prefix_len() == dest.prefix_len());
      columns_type(node_.page()).move_run_to(start, count, columns_type(dest.node_.page()), dest_start);
    } else {
      keys_and_columns().move_run_to(start, count, dest.keys_and_columns(), dest_start);
    }
  }
  window window_kept(std::size_t index) const noexcept {
    return {prefix_len(), columns_type(node_.page()).read_inline_key(index)};
  }

  // Moves slots: called by `split_insert`.
  separator<T> split_around(std::size_t separator_index, internal_node right) const noexcept {
    const std::size_t old_len = len();
    assert(separator_index < old_len);
    const std::size_t right_len = old_len - separator_index - 1;
    move_separators(separator_index + 1, right_len, right, 0);
    edge_slots().move_run_to(separator_index + 1, right_len + 1, right.edge_slots(), 0);
    right.node_.set_len(right_len);
    separator<T> up = read_separator(separator_index);
    set_len_refilling(separator_index);
    return up;
  }

  node_ptr<T> node_;
};

// --- The leaf operations that reach the parent ---

template <class T>
windowed_key<typename T::key_type> leaf_node<T>::replace_separator_key(internal_node<T> parent,
                                                                       std::size_t index,
                                                                       windowed_key<K>&& key,
                                                                       window fill) const noexcept {
  if constexpr (T::has_inline_keys) {
    const window kept = key.window;
    K old_head_key = head_key_slot().replace(0, std::move(key.key));
    parent.replace_separator(index, separator<T>(kept.moved_back_to(fill)));
    return {std::move(old_head_key), fill};
  } else {
    key.window = key.window.moved_back_to(fill);
    return parent.replace_separator(index, separator<T>(std::move(key))).take_key();
  }
}

template <class T>
void leaf_node<T>::steal_left(internal_node<T> parent, std::size_t index, leaf_node left, leaf_node right,
                              window fill) noexcept {
  const std::size_t left_len = left.len();
  const std::size_t right_len = right.len();
  windowed_key<K> key = left.keys_and_columns().remove(left_len - 1, left_len);
  V value = left.head_and_value_slots().remove(left_len, left_len + 1);
  right.head_and_value_slots().insert(0, right_len + 1, std::move(value));
  windowed_key<K> down = right.replace_separator_key(parent, index, std::move(key), fill);
  right.keys_and_columns().insert(0, right_len, std::move(down));
  left.set_len_refilling(left_len - 1);
  right.node_.set_len(right_len + 1);
}

template <class T>
void leaf_node<T>::steal_right(internal_node<T> parent, std::size_t index, leaf_node left, leaf_node right,
                               window fill) noexcept {
  const std::size_t left_len = left.len();
  const std::size_t right_len = right.len();
  windowed_key<K> key = right.keys_and_columns().remove(0, right_len);
  V value = right.head_and_value_slots().remove(0, right_len + 1);
  left.head_and_value_slots().insert(left_len + 1, left_len + 1, std::move(value));
  windowed_key<K> down = right.replace_separator_key(parent, index, std::move(key), fill);
  left.keys_and_columns().insert(left_len, left_len, std::move(down));
  left.node_.set_len(left_len + 1);
  right.set_len_refilling(right_len - 1);
}

}  // namespace bbtree::detail
