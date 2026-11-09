// `bbtree::unsync::bbtree_map`: an ordered map for one thread, a drop-in
// replacement for `std::map` (D1), on bbtree's B+-tree.
//
//   bbtree::unsync::bbtree_map<std::uint64_t, std::string> map;
//   map.insert({1, "one"});
//   map[2] = "two";
//   for (const auto& [key, value] : map) { ... }
//
// Where it differs from std::map (README.md, "Deviations"):
// - Iterators dereference to `entry_ref<K, V>`, a pair of references, not a
//   `std::pair<const K, V>&` (`entry_ref.hpp`).
// - Every insert and erase invalidates every iterator, as for
//   `absl::btree_map`: entries move between nodes.
// - Comparators are stateless; the allocator is a page allocator
//   (`allocator.hpp`), not a std allocator.
// - Keys and values must not throw from their move constructors and
//   destructors, nor comparators and inline-key specializations (D48).
// Extras from the Rust crate: `fold`, `first`/`last`, `pop_first`/`pop_last`,
// `retain`, `extract_if`, `append`, `split_off`, the batches `get_many` and
// `insert_many`, and `check_invariants`.
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/detail/tree/batch.hpp>
#include <bbtree/detail/tree/cursor.hpp>
#include <bbtree/detail/tree/tree.hpp>
#include <bbtree/detail/tree/tree_owner.hpp>
#include <bbtree/detail/tree/walk.hpp>
#include <bbtree/entry_ref.hpp>
#include <bbtree/node_config.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bbtree::unsync {

template <class K, class V, class Compare = std::less<K>, page_allocator Allocator = global_allocator,
          class Strategy = auto_inline_keys>
class bbtree_map : private detail::tree_owner<detail::tree_types<K, V, Compare, Strategy>, Allocator> {
  using types = detail::tree_types<K, V, Compare, Strategy>;
  using owner = detail::tree_owner<types, Allocator>;
  using owner::allocator_;
  using owner::length_;
  using owner::pages;
  using owner::tree_;
  using typename owner::cursor_type;

  // Whether lookups take other key types, as std::map's do with a
  // transparent comparator.
  template <class Q>
  static constexpr bool transparent = requires { typename Compare::is_transparent; } && !std::is_same_v<Q, K>;

 public:
  using key_type = K;
  using mapped_type = V;
  using value_type = std::pair<const K, V>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using key_compare = Compare;
  using allocator_type = Allocator;
  using reference = entry_ref<K, V>;
  using const_reference = entry_ref<K, const V>;

  template <bool Const>
  class basic_iterator {
    friend class bbtree_map;
    using value_ref = std::conditional_t<Const, const V, V>;

   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using iterator_concept = std::bidirectional_iterator_tag;
    using value_type = std::pair<const K, V>;
    using difference_type = std::ptrdiff_t;
    using reference = entry_ref<K, value_ref>;
    using pointer = entry_arrow<K, value_ref>;

    basic_iterator() noexcept = default;
    // An iterator converts to a const one. A template, so that it is never
    // the copy constructor.
    template <bool OtherConst>
      requires(Const && !OtherConst)
    basic_iterator(const basic_iterator<OtherConst>& other) noexcept  // NOLINT(google-explicit-constructor)
        : cursor_(other.cursor_) {}

    reference operator*() const noexcept { return {*cursor_.key(), *cursor_.value()}; }
    pointer operator->() const noexcept { return pointer(**this); }

    basic_iterator& operator++() noexcept {
      cursor_.next();
      return *this;
    }
    basic_iterator operator++(int) noexcept {
      basic_iterator before = *this;
      cursor_.next();
      return before;
    }
    basic_iterator& operator--() noexcept {
      cursor_.prev();
      return *this;
    }
    basic_iterator operator--(int) noexcept {
      basic_iterator before = *this;
      cursor_.prev();
      return before;
    }

    friend bool operator==(const basic_iterator& a, const basic_iterator& b) noexcept {
      return a.cursor_ == b.cursor_;
    }

   private:
    friend class basic_iterator<true>;
    explicit basic_iterator(cursor_type cursor) noexcept : cursor_(std::move(cursor)) {}

    cursor_type cursor_;
  };

  using iterator = basic_iterator<false>;
  using const_iterator = basic_iterator<true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  // --- Construction ---

  bbtree_map() noexcept = default;
  explicit bbtree_map(const Allocator& allocator) noexcept : owner(allocator) {}

  // Builds from the entries in [first, last), keeping the first of equal
  // keys as std::map does: sorted, then built full-node by full-node (D25).
  template <std::input_iterator It>
  bbtree_map(It first, It last, const Allocator& allocator = Allocator()) : owner(allocator) {
    insert(first, last);
  }
  bbtree_map(std::initializer_list<value_type> entries, const Allocator& allocator = Allocator())
      : owner(allocator) {
    insert(entries.begin(), entries.end());
  }

  // Copies are of the same shape, node by node; moves take the tree.
  bbtree_map(const bbtree_map&) = default;
  bbtree_map(bbtree_map&&) noexcept = default;
  bbtree_map& operator=(const bbtree_map&) = default;
  bbtree_map& operator=(bbtree_map&&) noexcept = default;
  bbtree_map& operator=(std::initializer_list<value_type> entries) {
    clear();
    insert(entries.begin(), entries.end());
    return *this;
  }
  ~bbtree_map() = default;

  allocator_type get_allocator() const noexcept { return allocator_; }
  key_compare key_comp() const noexcept { return key_compare(); }

  // --- Iterators ---

  iterator begin() noexcept { return iterator(cursor_type::first(&tree_)); }
  const_iterator begin() const noexcept { return const_iterator(cursor_type::first(&tree_)); }
  const_iterator cbegin() const noexcept { return begin(); }
  iterator end() noexcept { return iterator(cursor_type(&tree_)); }
  const_iterator end() const noexcept { return const_iterator(cursor_type(&tree_)); }
  const_iterator cend() const noexcept { return end(); }
  reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
  reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

  // Runs `visit(key, value)` on every entry in key order, a leaf at a time:
  // faster than iterators, whose steps keep their place in memory (P16).
  template <class Visit>
  void for_each(Visit&& visit) {
    detail::in_order_walk<types>(tree_).run(visit);
  }
  template <class Visit>
  void for_each(Visit&& visit) const {
    auto const_value = [&](const K& key, V& value) { visit(key, std::as_const(value)); };
    detail::in_order_walk<types>(tree_).run(const_value);
  }

  // `fold(accumulator, key, value)` over every entry in key order, from
  // `init`, returning the last accumulator: Rust's `iter().fold`. Prefer it
  // to a `for_each` whose visitor accumulates into a captured variable,
  // which stays in memory (PORTING.md, P16).
  template <class A, class Fold>
  A fold(A init, Fold&& fold) const {
    auto const_value = [&fold](A accumulator, const K& key, V& value) {
      return fold(std::move(accumulator), key, std::as_const(value));
    };
    return detail::in_order_walk<types>(tree_).fold(std::move(init), const_value);
  }

  // --- Capacity ---

  [[nodiscard]] bool empty() const noexcept { return length_ == 0; }
  size_type size() const noexcept { return length_; }
  size_type max_size() const noexcept { return static_cast<size_type>(PTRDIFF_MAX); }

  // --- Lookup ---

  iterator find(const K& key) noexcept { return found_or_end(tree_.find(key)); }
  const_iterator find(const K& key) const noexcept {
    return const_iterator(const_cast<bbtree_map*>(this)->find(key));
  }
  template <class Q>
    requires transparent<Q>
  iterator find(const Q& key) noexcept {
    return found_or_end(tree_.find(key));
  }
  template <class Q>
    requires transparent<Q>
  const_iterator find(const Q& key) const noexcept {
    return const_iterator(const_cast<bbtree_map*>(this)->find(key));
  }

  bool contains(const K& key) const noexcept { return tree_.find(key).has_value(); }
  template <class Q>
    requires transparent<Q>
  bool contains(const Q& key) const noexcept {
    return tree_.find(key).has_value();
  }
  size_type count(const K& key) const noexcept { return contains(key) ? 1 : 0; }
  template <class Q>
    requires transparent<Q>
  size_type count(const Q& key) const noexcept {
    return contains(key) ? 1 : 0;
  }

  // The value at `key`, if any: the Rust crate's `get`, without an iterator.
  V* get(const K& key) noexcept {
    const auto found = tree_.find(key);
    return found ? found->value() : nullptr;
  }
  const V* get(const K& key) const noexcept { return const_cast<bbtree_map*>(this)->get(key); }
  template <class Q>
    requires transparent<Q>
  V* get(const Q& key) noexcept {
    const auto found = tree_.find(key);
    return found ? found->value() : nullptr;
  }
  template <class Q>
    requires transparent<Q>
  const V* get(const Q& key) const noexcept {
    return const_cast<bbtree_map*>(this)->get(key);
  }

  V& at(const K& key) {
    if (V* value = get(key)) {
      return *value;
    }
    throw std::out_of_range("bbtree_map::at: no such key");
  }
  const V& at(const K& key) const { return const_cast<bbtree_map*>(this)->at(key); }

  V& operator[](const K& key)
    requires std::is_default_constructible_v<V>
  {
    return *try_emplace(key).first.cursor_.value();
  }
  V& operator[](K&& key)
    requires std::is_default_constructible_v<V>
  {
    return *try_emplace(std::move(key)).first.cursor_.value();
  }

  iterator lower_bound(const K& key) noexcept {
    return iterator(cursor_type::beside(&tree_, key, detail::key_side::before));
  }
  const_iterator lower_bound(const K& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::before));
  }
  iterator upper_bound(const K& key) noexcept {
    return iterator(cursor_type::beside(&tree_, key, detail::key_side::after));
  }
  const_iterator upper_bound(const K& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::after));
  }
  template <class Q>
    requires transparent<Q>
  iterator lower_bound(const Q& key) noexcept {
    return iterator(cursor_type::beside(&tree_, key, detail::key_side::before));
  }
  template <class Q>
    requires transparent<Q>
  const_iterator lower_bound(const Q& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::before));
  }
  template <class Q>
    requires transparent<Q>
  iterator upper_bound(const Q& key) noexcept {
    return iterator(cursor_type::beside(&tree_, key, detail::key_side::after));
  }
  template <class Q>
    requires transparent<Q>
  const_iterator upper_bound(const Q& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::after));
  }

  std::pair<iterator, iterator> equal_range(const K& key) noexcept {
    return {lower_bound(key), upper_bound(key)};
  }
  std::pair<const_iterator, const_iterator> equal_range(const K& key) const noexcept {
    return {lower_bound(key), upper_bound(key)};
  }

  // --- Batches (D53) ---

  // The value of each of `keys`, or null, in input order: looked up together,
  // so that one key's misses overlap the others'.
  template <class Q, std::size_t N>
    requires(std::is_same_v<Q, K> || transparent<Q>)
  std::array<V*, N> get_many(const std::array<Q, N>& keys) noexcept {
    std::array<const Q*, N> pointers;
    for (std::size_t index = 0; index < N; ++index) {
      pointers[index] = &keys[index];
    }
    std::array<V*, N> values;
    get_many(std::span<const Q* const>(pointers), std::span<V*>(values));
    return values;
  }

  // The same for any number of keys, by pointer, into `values`.
  template <class Q>
    requires(std::is_same_v<Q, K> || transparent<Q>)
  void get_many(std::span<const Q* const> keys, std::span<V*> values) noexcept {
    std::optional<detail::found_entry<types>> found[detail::batch_lookup<types>::max_lanes];
    for (std::size_t start = 0; start < keys.size(); start += std::size(found)) {
      const std::size_t count = std::min(std::size(found), keys.size() - start);
      detail::batch_lookup<types>::find_many(
          tree_, length_, keys.subspan(start, count),
          std::span<std::optional<detail::found_entry<types>>>(found, count));
      for (std::size_t index = 0; index < count; ++index) {
        values[start + index] = found[index] ? found[index]->value() : nullptr;
      }
    }
  }

  // Inserts or assigns each entry in input order, as `insert_or_assign` one
  // by one would, the keys' paths fetched together first; returns whether
  // each was new (the Rust crate's `insert_many`).
  template <std::size_t N>
  std::array<bool, N> insert_or_assign_many(std::array<std::pair<K, V>, N>&& entries) {
    std::array<const K*, N> keys;
    for (std::size_t index = 0; index < N; ++index) {
      keys[index] = &entries[index].first;
    }
    std::array<V*, N> ignored;
    get_many(std::span<const K* const>(keys), std::span<V*>(ignored));
    std::array<bool, N> inserted;
    for (std::size_t index = 0; index < N; ++index) {
      inserted[index] =
          insert_or_assign(std::move(entries[index].first), std::move(entries[index].second)).second;
    }
    return inserted;
  }

  // The first and last entries, if any.
  std::optional<const_reference> first() const noexcept { return entry_of(tree_.first()); }
  std::optional<const_reference> last() const noexcept { return entry_of(tree_.last()); }

  // --- Inserting ---

  std::pair<iterator, bool> insert(const value_type& entry) { return try_emplace(entry.first, entry.second); }
  // The key is const in a value_type, so it is copied; the value moves.
  std::pair<iterator, bool> insert(value_type&& entry) {
    return try_emplace(entry.first, std::move(entry.second));
  }
  template <class P>
    requires std::is_constructible_v<value_type, P&&>
  std::pair<iterator, bool> insert(P&& entry) {
    return emplace(std::forward<P>(entry));
  }
  iterator insert(const_iterator /*hint*/, const value_type& entry) { return insert(entry).first; }
  iterator insert(const_iterator /*hint*/, value_type&& entry) { return insert(std::move(entry)).first; }
  // Inserts the entries in [first, last), keeping existing keys and the
  // first of equal new ones. Into an empty map, by a bulk build (D25).
  template <std::input_iterator It>
  void insert(It first, It last) {
    if (!empty()) {
      for (; first != last; ++first) {
        emplace(*first);
      }
      return;
    }
    if constexpr (std::forward_iterator<It>) {
      auto out_of_order = [](const auto& a, const auto& b) { return !key_order::less(a.first, b.first); };
      if (std::adjacent_find(first, last, out_of_order) == last) {
        this->build_ascending(
            first, last, [](auto&& entry) { return std::pair<K, V>(std::forward<decltype(entry)>(entry)); });
        return;
      }
    }
    std::vector<std::pair<K, V>> entries;
    if constexpr (std::forward_iterator<It>) {
      entries.reserve(static_cast<std::size_t>(std::distance(first, last)));
    }
    bool ascending = true;
    for (; first != last; ++first) {
      entries.emplace_back(*first);
      if (entries.size() > 1 && !key_order::less(entries[entries.size() - 2].first, entries.back().first)) {
        ascending = false;
      }
    }
    this->build_from(entries, ascending);
  }
  void insert(std::initializer_list<value_type> entries) { insert(entries.begin(), entries.end()); }

  template <class M>
  std::pair<iterator, bool> insert_or_assign(const K& key, M&& value) {
    return put(key, [&] { return V(std::forward<M>(value)); }, true);
  }
  template <class M>
  std::pair<iterator, bool> insert_or_assign(K&& key, M&& value) {
    return put(std::move(key), [&] { return V(std::forward<M>(value)); }, true);
  }
  template <class M>
  iterator insert_or_assign(const_iterator /*hint*/, const K& key, M&& value) {
    return insert_or_assign(key, std::forward<M>(value)).first;
  }
  template <class M>
  iterator insert_or_assign(const_iterator /*hint*/, K&& key, M&& value) {
    return insert_or_assign(std::move(key), std::forward<M>(value)).first;
  }

  template <class... Args>
  std::pair<iterator, bool> try_emplace(const K& key, Args&&... args) {
    return put(key, [&] { return V(std::forward<Args>(args)...); }, false);
  }
  template <class... Args>
  std::pair<iterator, bool> try_emplace(K&& key, Args&&... args) {
    return put(std::move(key), [&] { return V(std::forward<Args>(args)...); }, false);
  }
  template <class... Args>
  iterator try_emplace(const_iterator /*hint*/, const K& key, Args&&... args) {
    return try_emplace(key, std::forward<Args>(args)...).first;
  }
  template <class... Args>
  iterator try_emplace(const_iterator /*hint*/, K&& key, Args&&... args) {
    return try_emplace(std::move(key), std::forward<Args>(args)...).first;
  }

  // Constructs the entry first, as std::map's emplace does, then keeps it only
  // if its key is new.
  template <class... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    std::pair<K, V> entry(std::forward<Args>(args)...);
    return put(std::move(entry.first), [&] { return std::move(entry.second); }, false);
  }
  template <class... Args>
  iterator emplace_hint(const_iterator /*hint*/, Args&&... args) {
    return emplace(std::forward<Args>(args)...).first;
  }

  // --- Erasing ---

  size_type erase(const K& key) noexcept { return tree_.remove(key, pages(), length_) ? 1 : 0; }
  template <class Q>
    requires transparent<Q>
  size_type erase(const Q& key) noexcept {
    return tree_.remove(key, pages(), length_) ? 1 : 0;
  }

  // Erases the entry at `position` and returns the next.
  iterator erase(const_iterator position) noexcept {
    cursor_type at = position.cursor_;
    detail::node_stack_mut<types> stack(tree_, at.attached_stack());
    stack.remove_and_go_to_next(pages(), length_);
    return iterator(cursor_type(&tree_, stack.stack()));
  }
  iterator erase(iterator position) noexcept { return erase(const_iterator(position)); }

  iterator erase(const_iterator first, const_iterator last) noexcept {
    if (last == end()) {
      while (first != last) {
        first = erase(first);
      }
      return end();
    }
    // Erasing moves entries, so the last iterator goes stale: count instead.
    std::size_t count = 0;
    for (const_iterator at = first; at != last; ++at) {
      ++count;
    }
    iterator at(first.cursor_);
    for (; count > 0; --count) {
      at = erase(at);
    }
    return at;
  }

  // Removes and returns the first or last entry, if any.
  std::optional<std::pair<K, V>> pop_first() noexcept { return tree_.pop_first(pages(), length_); }
  std::optional<std::pair<K, V>> pop_last() noexcept { return tree_.pop_last(pages(), length_); }

  // Removes and returns the entry at `key`, if any: the Rust crate's
  // `remove_entry`.
  std::optional<std::pair<K, V>> remove(const K& key) noexcept { return tree_.remove(key, pages(), length_); }

  void clear() noexcept { this->clear_tree(); }
  void swap(bbtree_map& other) noexcept { this->swap_with(other); }
  friend void swap(bbtree_map& a, bbtree_map& b) noexcept { a.swap(b); }

  // Keeps only the entries for which `keep(key, value)` holds, visiting each
  // once in order: the Rust crate's `retain`, std's `erase_if`.
  template <class Keep>
  size_type retain(Keep&& keep) {
    const size_type before = length_;
    iterator at = begin();
    while (at != end()) {
      if (keep(*at.cursor_.key(), *at.cursor_.value())) {
        ++at;
      } else {
        at = erase(at);
      }
    }
    return before - length_;
  }
  template <class Predicate>
  friend size_type erase_if(bbtree_map& map, Predicate predicate) {
    return map.retain([&](const K& key, V& value) { return !predicate(entry_ref<K, V>{key, value}); });
  }

  // Moves every entry of `other` into this map, `other`'s value replacing
  // this map's at an equal key, as Rust's `append` does; `other` is empty
  // after. Both maps' entries move in order into one bulk build (D25), in
  // O(n + m).
  void append(bbtree_map& other) { this->append_from(other); }

  // Moves the entries from `key` on into a new map, which it returns: a cut
  // along the search's node stack, O(log n) (D25).
  bbtree_map split_off(const K& key) {
    bbtree_map right(allocator_);
    this->split_off_into(key, right);
    return right;
  }

  // --- Comparison ---

  friend bool operator==(const bbtree_map& a, const bbtree_map& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const_reference x, const_reference y) {
             return x.first == y.first && x.second == y.second;
           });
  }
  friend auto operator<=>(const bbtree_map& a, const bbtree_map& b)
    requires std::three_way_comparable<K> && std::three_way_comparable<V>
  {
    using ordering = std::common_comparison_category_t<std::compare_three_way_result_t<K>,
                                                       std::compare_three_way_result_t<V>>;
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end(),
                                                  [](const_reference x, const_reference y) -> ordering {
                                                    if (auto order = x.first <=> y.first; order != 0) {
                                                      return order;
                                                    }
                                                    return x.second <=> y.second;
                                                  });
  }

  // Panics, by `detail::invariant_violation`, unless the tree is well formed;
  // where `BBTREE_CHECKED` is set (tests, fuzz targets).
  void check_invariants() const { tree_.check_invariants(length_); }

 private:
  using key_order = typename types::order;

  iterator found_or_end(std::optional<detail::found_entry<types>> found) noexcept {
    return found ? iterator(cursor_type(&tree_, *found)) : end();
  }

  std::optional<const_reference> entry_of(std::optional<detail::found_entry<types>> found) const noexcept {
    if (!found) {
      return std::nullopt;
    }
    return const_reference{*found->key(), *found->value()};
  }

  template <class KeyRef, class MakeValue>
  std::pair<iterator, bool> put(KeyRef&& key, MakeValue&& make_value, bool assign) {
    const auto inserted = tree_.insert(std::forward<KeyRef>(key), std::forward<MakeValue>(make_value), assign,
                                       pages(), length_);
    return {iterator(cursor_type(&tree_, inserted.entry)), inserted.inserted};
  }
};

}  // namespace bbtree::unsync
