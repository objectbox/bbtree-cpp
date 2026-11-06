// Leaves: the nodes that hold the entries (D9, D17, D29). A leaf's page holds
// its keys and values, each key paired with its value with inline keys, the
// inline keys a search runs over first (D18), and in a leaf of heap byte
// strings, the keys' hash tags (D36). It also holds a head: the value of the
// separator just left of it, and with inline keys that separator's key too
// (D49). Every leaf but the tree's leftmost has one.
#pragma once

#include <bbtree/detail/node/child.hpp>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>

namespace bbtree::detail {

template <class T>
class internal_node;

// What the leaf right of a separator being removed gives up: its head's value
// (and with inline keys its key), and its first key, handed up as the
// separator taking the removed one's place (D17).
template <class T>
struct head_succession {
  std::optional<typename T::key_type> removed_key;
  typename T::mapped_type value;
  separator<T> successor;
};

template <class T>
class leaf_node : public child_node<T, leaf_node<T>, true> {
  using K = typename T::key_type;
  using V = typename T::mapped_type;
  using base = child_node<T, leaf_node<T>, true>;

 public:
  using types = T;
  using columns_type = typename base::columns_type;
  static constexpr leaf_layout layout = T::leaf;
  static constexpr std::size_t capacity = base::capacity;
  using page_type = fresh_page<T, page_role::leaf>;

  leaf_node() noexcept = default;
  explicit leaf_node(node_ptr<T> node) noexcept : node_(node) {
    assert(node.is_leaf() && "the node is internal");
  }

  // A new, empty leaf without a head, at `prefix_len`.
  static leaf_node create(page_type&& page, std::size_t prefix_len) noexcept {
    const leaf_node leaf(node_ptr<T>::template initialize<true>(std::move(page).release()));
    leaf.node_.set_prefix_len(prefix_len);
    return leaf;
  }

  // Gives the leaf's page back; every key, value and head has moved out.
  page_type free() const noexcept { return page_type::adopt(node_.page()); }

  node_ptr<T> node() const noexcept { return node_; }
  std::size_t len() const noexcept { return node_.len(); }
  bool has_head() const noexcept { return node_.has_head(); }
  std::size_t prefix_len() const noexcept { return node_.prefix_len(); }
  friend bool operator==(leaf_node, leaf_node) = default;

  key_slots<T, true> keys_and_columns() const noexcept { return {node_.page(), prefix_len()}; }
  slots<K> key_slots_only() const noexcept {
    return {node_.page() + layout.keys.keys_offset, layout.keys.key_stride};
  }
  slots<V> value_slots() const noexcept {
    return {node_.page() + layout.values_offset(), layout.value_stride};
  }
  // Slot 0 is the head, slot i + 1 value i.
  slots<V> head_and_value_slots() const noexcept {
    return {node_.page() + layout.head_offset, layout.value_stride};
  }
  // The head's key, with inline keys (D49).
  slots<K> head_key_slot() const noexcept
    requires T::has_inline_keys
  {
    return {node_.page() + *layout.head_key_offset, 0};
  }

  const K& key(std::size_t index) const noexcept {
    assert(index < len() && "key index out of bounds");
    return *key_slots_only().slot(index);
  }
  V& value(std::size_t index) const noexcept {
    assert(index < len() && "entry index out of bounds");
    return *value_slots().slot(index);
  }
  K* key_address(std::size_t index) const noexcept { return key_slots_only().slot(index); }
  V* value_address(std::size_t index) const noexcept { return value_slots().slot(index); }

  // The inline keys, read in place.
  auto inline_keys() const noexcept
    requires columns_type::has_inline_keys
  {
    return columns_type(node_.page()).inline_keys(len());
  }

  // Key `index`'s window at the leaf's prefix length.
  window window_at(std::size_t index) const noexcept {
    assert(index < len());
    return {prefix_len(), columns_type(node_.page()).read_inline_key(index)};
  }

  // As far as its fences share: nothing below a leaf bounds it.
  std::size_t allowed_prefix_len(std::size_t prefix_len) const noexcept { return prefix_len; }

  // --- Searching ---

  // Binary search for `key` among the leaf's keys, over the inline keys first
  // where the leaf and the query have them. In bounds for any order.
  template <class Q>
  [[gnu::always_inline]] search_result search(const search_key<T, Q>& key) const noexcept {
    if constexpr (columns_type::has_inline_keys && search_key<T, Q>::compares_inline) {
      const window target = key.inline_target(prefix_len());
      const slot_range candidates = inline_keys().equal_range(target.inline_key());
      if (candidates.empty()) {
        return {candidates.start, false};
      }
      return search_candidates(candidates, key, target);
    } else {
      return search_all_keys(key);
    }
  }

  // The index of the key equal to `key`, if any: an exact-key lookup, which
  // may reject candidates by hash tag (D36) and confirm a lone candidate by
  // equality alone (D38).
  template <class Q>
  [[gnu::always_inline]] std::optional<std::size_t> find(const search_key<T, Q>& key) const noexcept {
    if constexpr (columns_type::has_inline_keys && search_key<T, Q>::compares_inline) {
      const window target = key.inline_target(prefix_len());
      const slot_range candidates = inline_keys().equal_range(target.inline_key());
      if (candidates.empty()) {
        return std::nullopt;
      }
      if (candidates.size() == 1) {
        return confirms(key, target, candidates.start) ? std::optional<std::size_t>(candidates.start)
                                                       : std::nullopt;
      }
      if constexpr (search_key<T, Q>::has_hash_tag && columns_type::has_hash_tags) {
        const tag_matches matches = columns_type(node_.page()).tags().matching(candidates, key.hash_tag());
        switch (matches.found) {
          case tag_matches::count::none:
            return std::nullopt;
          case tag_matches::count::one:
            return tied_at(key, target, matches.lower) == 0 ? std::optional<std::size_t>(matches.lower)
                                                            : std::nullopt;
          case tag_matches::count::two: {
            const auto upper = tied_at(key, target, matches.upper);
            if (upper == 0) {
              return matches.upper;
            }
            if (upper < 0 && tied_at(key, target, matches.lower) == 0) {
              return matches.lower;
            }
            return std::nullopt;
          }
          case tag_matches::count::many:
            break;
        }
      }
      const search_result found = search_candidates(candidates, key, target);
      return found.found ? std::optional<std::size_t>(found.index) : std::nullopt;
    } else {
      const search_result found = search_all_keys(key);
      return found.found ? std::optional<std::size_t>(found.index) : std::nullopt;
    }
  }

  // --- The head (D17, D49) ---

  V& head_value() const noexcept {
    assert(has_head() && "the leaf has no head");
    return *head_and_value_slots().slot(0);
  }
  V* head_value_address() const noexcept { return head_and_value_slots().slot(0); }

  const K& head_key() const noexcept
    requires T::has_inline_keys
  {
    assert(has_head() && "the leaf has no head");
    return *head_key_slot().slot(0);
  }

  // Sets a headless leaf's head: `value`, and with inline keys `key`.
  void put_head(std::optional<K>&& key, V&& value) const noexcept {
    assert(!has_head() && "the leaf has a head");
    if constexpr (T::has_inline_keys) {
      assert(key.has_value());
      head_key_slot().construct(0, std::move(*key));
    } else {
      assert(!key.has_value());
    }
    head_and_value_slots().construct(0, std::move(value));
    node_.set_has_head(true);
  }

  // Makes `key`, the separator just left of this headless leaf, its head with
  // `value`, and returns the separator as its internal node keeps it.
  separator<T> put_separator_head(windowed_key<K>&& key, V&& value) const noexcept {
    if constexpr (T::has_inline_keys) {
      const window kept = key.window;
      put_head(std::move(key.key), std::move(value));
      return separator<T>(kept);
    } else {
      put_head(std::nullopt, std::move(value));
      return separator<T>(std::move(key));
    }
  }

  // Takes the head out: its value, and with inline keys its key.
  std::pair<std::optional<K>, V> take_head() const noexcept {
    assert(has_head() && "the leaf has no head");
    std::optional<K> key;
    if constexpr (T::has_inline_keys) {
      key.emplace(head_key_slot().take(0));
    }
    V value = head_and_value_slots().take(0);
    node_.set_has_head(false);
    return {std::move(key), std::move(value)};
  }

  K replace_head_key(K&& key) const noexcept
    requires T::has_inline_keys
  {
    assert(has_head());
    return head_key_slot().replace(0, std::move(key));
  }

  // Gives up the head, the separator's entry, and hands up the first key as
  // the separator taking its place; the first key's entry becomes the head.
  // Nothing if the leaf has no keys.
  std::optional<head_succession<T>> take_head_and_first_key() const noexcept {
    assert(has_head());
    const std::size_t old_len = len();
    if (old_len == 0) {
      return std::nullopt;
    }
    windowed_key<K> first = keys_and_columns().remove(0, old_len);
    V value = head_and_value_slots().remove(0, old_len + 1);
    set_len_refilling(old_len - 1);
    if constexpr (T::has_inline_keys) {
      K removed = head_key_slot().replace(0, std::move(first.key));
      return head_succession<T>{std::move(removed), std::move(value), separator<T>(first.window)};
    } else {
      return head_succession<T>{std::nullopt, std::move(value), separator<T>(std::move(first))};
    }
  }

  // --- Changing entries ---

  // Inserts `key` and `value` at edge `index`; the leaf has room.
  void insert_fit(std::size_t index, windowed_key<K>&& key, V&& value) const noexcept {
    const std::size_t old_len = len();
    assert(index <= old_len && old_len < capacity);
    if constexpr (T::paired_leaf_entries && slots<K>::moves_as_bytes && slots<V>::moves_as_bytes) {
      // Shift whole entries, key and value, in one copy (D29).
      const key_slots<T, true> keys = keys_and_columns();
      keys.columns().insert(index, old_len, key.window.inline_key(), tag_of(key.key));
      entry_shift::up(node_.page() + layout.keys.keys_offset, layout.keys.key_stride, index, old_len);
      keys.keys().construct(index, std::move(key.key));
      value_slots().construct(index, std::move(value));
    } else {
      keys_and_columns().insert(index, old_len, std::move(key));
      value_slots().insert(index, old_len, std::move(value));
    }
    node_.set_len(old_len + 1);
  }

  // Splits the full leaf and inserts at edge `index`, on the side where that
  // edge ended up. The separator goes up; its entry becomes the right leaf's
  // head (D17, D49).
  split_result<T, leaf_node> split_insert(std::size_t index, windowed_key<K>&& key, V&& value,
                                          page_type&& right_page) const noexcept {
    assert(len() == capacity);
    const auto [separator_index, inserted] = insertion_side::of_split(capacity, index, base::min_len);
    const leaf_node right = create(std::move(right_page), prefix_len());
    separator<T> middle = split_around(separator_index, right);
    (inserted.right ? right : *this).insert_fit(inserted.index, std::move(key), std::move(value));
    return {std::move(middle), right, inserted};
  }

  // Removes entry `index`, shifting later ones left.
  std::pair<K, V> remove(std::size_t index) const noexcept {
    const std::size_t old_len = len();
    assert(index < old_len && "entry index out of bounds");
    if constexpr (T::paired_leaf_entries && slots<K>::moves_as_bytes && slots<V>::moves_as_bytes) {
      const key_slots<T, true> keys = keys_and_columns();
      V value = value_slots().take(index);
      K key = keys.keys().take(index);
      keys.columns().remove(index, old_len);
      entry_shift::down(node_.page() + layout.keys.keys_offset, layout.keys.key_stride, index, old_len);
      set_len_refilling(old_len - 1);
      return {std::move(key), std::move(value)};
    } else {
      K key = std::move(keys_and_columns().remove(index, old_len).key);
      V value = value_slots().remove(index, old_len);
      set_len_refilling(old_len - 1);
      return {std::move(key), std::move(value)};
    }
  }

  // Replaces key `index` and its inline key; the caller keeps keys in order.
  K replace_key(std::size_t index, windowed_key<K>&& key) const noexcept {
    assert(index < len());
    return std::move(keys_and_columns().replace(index, std::move(key)).key);
  }

  // Cuts the leaf at edge `index`: the entries from `index` on move into a
  // new headless leaf on `page`, returned; this leaf keeps the rest and its
  // head.
  leaf_node cut_off_at(std::size_t index, page_type&& page) const noexcept {
    const std::size_t old_len = len();
    assert(index <= old_len && "cutting a leaf past its last edge");
    const std::size_t count = old_len - index;
    const leaf_node right = create(std::move(page), prefix_len());
    keys_and_columns().move_run_to(index, count, right.keys_and_columns(), 0);
    value_slots().move_run_to(index, count, right.value_slots(), 0);
    right.set_len_refilling(count);
    set_len_refilling(index);
    return right;
  }

  // --- Balancing with a sibling (`siblings.hpp`) ---

  // Moves the left sibling's last entry to the right one's front, through
  // the parent's separator `index`; `fill` is that separator's window, at
  // the parent's prefix length, which the right child is at already.
  static void steal_left(internal_node<T> parent, std::size_t index, leaf_node left, leaf_node right,
                         window fill) noexcept;
  static void steal_right(internal_node<T> parent, std::size_t index, leaf_node left, leaf_node right,
                          window fill) noexcept;

  // Puts the separator at `left`'s end, and every entry of `right` after it;
  // `right`'s head rejoins as the separator's entry. Both at `fill`'s prefix.
  static void append(leaf_node left, window fill, separator<T>&& middle, leaf_node right) noexcept {
    const std::size_t left_len = left.len();
    const std::size_t right_len = right.len();
    windowed_key<K> key = [&] {
      if constexpr (T::has_inline_keys) {
        (void)middle;
        return windowed_key<K>{right.head_key_slot().take(0), fill};
      } else {
        return std::move(middle).take_key();
      }
    }();
    left.keys_and_columns().write(left_len, std::move(key));
    right.keys_and_columns().move_run_to(0, right_len, left.keys_and_columns(), left_len + 1);
    right.head_and_value_slots().move_run_to(0, right_len + 1, left.head_and_value_slots(), left_len + 1);
    left.node_.set_len(left_len + 1 + right_len);
  }

  // Moves this leaf whole into the page at `dest`, a page as large as a
  // leaf's, which holds nothing; returns it there.
  leaf_node move_to(std::byte* dest) const noexcept {
    if constexpr (slots<K>::moves_as_bytes && slots<V>::moves_as_bytes) {
      page_copy<T>::keeping_latch(dest, node_.page(), layout.page.size);
      return leaf_node(node_ptr<T>::leaf_at(dest));
    } else {
      const leaf_node moved(node_ptr<T>::template initialize<true>(dest));
      moved.node_.set_prefix_len(prefix_len());
      const std::size_t count = len();
      keys_and_columns().move_run_to(0, count, moved.keys_and_columns(), 0);
      head_and_value_slots().move_run_to(has_head() ? 0 : 1, has_head() ? count + 1 : count,
                                         moved.head_and_value_slots(), has_head() ? 0 : 1);
      if constexpr (T::has_inline_keys) {
        if (has_head()) {
          head_key_slot().move_run_to(0, 1, moved.head_key_slot(), 0);
        }
      }
      moved.node_.set_has_head(has_head());
      moved.node_.set_len(count);
      return moved;
    }
  }

  // Sets the length; vacated inline-key slots get the greatest inline key
  // again (invariant 4).
  void set_len_refilling(std::size_t new_len) const noexcept {
    assert(new_len <= capacity);
    columns_type(node_.page()).mark_vacant(new_len, len());
    node_.set_len(new_len);
  }

 private:
  static std::uint8_t tag_of(const K& key) noexcept {
    if constexpr (columns_type::has_hash_tags) {
      return T::inliner::hash_tag(key);
    } else {
      (void)key;
      return 0;
    }
  }

  template <class Q>
  search_result search_all_keys(const search_key<T, Q>& key) const noexcept {
    const node_keys<K> keys(key_slots_only(), 0, len());
    if constexpr (base::plan.all_keys_search == key_search::branchless) {
      return key_list_search::branchless<T>(keys, key.key());
    } else {
      return key_list_search::early_exit(keys,
                                         [&](const K& probe) { return T::order::cmp(key.key(), probe); });
    }
  }

  // The search among the candidates whose inline keys tie with the query's;
  // the query's place lies among them (the inline-key contract).
  template <class Q>
  search_result search_candidates(slot_range candidates, const search_key<T, Q>& key,
                                  window target) const noexcept {
    const node_keys<K> keys(key_slots_only(), candidates.start, candidates.size());
    search_result found;
    if constexpr (base::plan.candidate_search == key_search::branchless) {
      found = key_list_search::branchless<T>(keys, key.key());
    } else {
      found = key_list_search::early_exit(keys, [&](const K& stored) { return tied(key, target, stored); });
    }
    found.index += candidates.start;
    return found;
  }

  template <class Q>
  std::weak_ordering tied(const search_key<T, Q>& key, window target, const K& stored) const noexcept {
    return key.cmp_tied(*base::plan.width, target, stored);
  }
  template <class Q>
  std::weak_ordering tied_at(const search_key<T, Q>& key, window target, std::size_t index) const noexcept {
    return tied(key, target, *key_slots_only().slot(index));
  }

  // Whether candidate `index`, the only one, is the key.
  template <class Q>
  bool confirms(const search_key<T, Q>& key, window target, std::size_t index) const noexcept {
    const K& stored = *key_slots_only().slot(index);
    if constexpr (base::plan.whole_key_equality && search_key<T, Q>::compares_by_bytes) {
      return key.equals_whole_bytes(stored);
    } else {
      return tied(key, target, stored) == 0;
    }
  }

  // Moves the entries right of `separator_index` into `right`, a fresh leaf
  // at this leaf's prefix length, and returns key `separator_index` as the
  // parent's separator; its entry becomes `right`'s head.
  separator<T> split_around(std::size_t separator_index, leaf_node right) const noexcept {
    const std::size_t old_len = len();
    assert(separator_index < old_len);
    const std::size_t right_len = old_len - separator_index - 1;
    keys_and_columns().move_run_to(separator_index + 1, right_len, right.keys_and_columns(), 0);
    head_and_value_slots().move_run_to(separator_index + 1, right_len + 1, right.head_and_value_slots(), 0);
    windowed_key<K> middle = keys_and_columns().read(separator_index);
    right.node_.set_has_head(true);
    right.set_len_refilling(right_len);
    set_len_refilling(separator_index);
    if constexpr (T::has_inline_keys) {
      right.head_key_slot().construct(0, std::move(middle.key));
      return separator<T>(middle.window);
    } else {
      return separator<T>(std::move(middle));
    }
  }

  // Puts `key`, leaving a sibling, in place of the key of the parent's
  // separator `index`, and returns the old key with `fill` as its window.
  // This leaf is the leftmost right of that separator, with a head.
  windowed_key<K> replace_separator_key(internal_node<T> parent, std::size_t index, windowed_key<K>&& key,
                                        window fill) const noexcept;

  node_ptr<T> node_;
};

}  // namespace bbtree::detail
