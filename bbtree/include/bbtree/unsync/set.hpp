// `bbtree::unsync::bbtree_set`: an ordered set for one thread, a drop-in
// replacement for `std::set` (D1), on the map's tree with values that take no
// room.
//
// Iterators dereference to `const K&`: a set's key has an address of its own,
// as std::set's does. Every insert and erase invalidates every iterator.
// Extras from the Rust crate: `first`/`last`, `pop_first`/`pop_last`,
// `retain`, `append`, `split_off`, `is_subset`/`is_superset`/`is_disjoint`,
// and the set algebra `|`, `&`, `-`, `^`, whose results are built on the left
// set's allocator.
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/detail/tree/batch.hpp>
#include <bbtree/detail/tree/cursor.hpp>
#include <bbtree/detail/tree/reverse_iterator.hpp>
#include <bbtree/detail/tree/tree.hpp>
#include <bbtree/detail/tree/tree_owner.hpp>
#include <bbtree/detail/tree/walk.hpp>
#include <bbtree/node_config.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace bbtree {
namespace detail {

// A set's value: nothing, in no room (D9).
struct set_value {
  friend bool operator==(set_value, set_value) noexcept { return true; }
};

}  // namespace detail

namespace unsync {

template <class K, class Compare = std::less<K>, page_allocator Allocator = global_allocator,
          class Strategy = auto_inline_keys>
class bbtree_set
    : private detail::tree_owner<detail::tree_types<K, detail::set_value, Compare, Strategy>, Allocator> {
  using types = detail::tree_types<K, detail::set_value, Compare, Strategy>;
  using owner = detail::tree_owner<types, Allocator>;
  using owner::allocator_;
  using owner::length_;
  using owner::pages;
  using owner::tree_;
  using typename owner::cursor_type;
  using key_order = typename types::order;

  template <class Q>
  static constexpr bool transparent = requires { typename Compare::is_transparent; } && !std::is_same_v<Q, K>;

 public:
  using key_type = K;
  using value_type = K;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using key_compare = Compare;
  using value_compare = Compare;
  using allocator_type = Allocator;
  using reference = const K&;
  using const_reference = const K&;

  class const_iterator {
    friend class bbtree_set;

   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using iterator_concept = std::bidirectional_iterator_tag;
    using value_type = K;
    using difference_type = std::ptrdiff_t;
    using reference = const K&;
    using pointer = const K*;

    const_iterator() noexcept = default;

    reference operator*() const noexcept { return *cursor_.key(); }
    pointer operator->() const noexcept { return cursor_.key(); }

    // Inlined into the loop that steps, as the map's (`map.hpp`).
    [[gnu::always_inline]] const_iterator& operator++() noexcept {
      cursor_.next();
      return *this;
    }
    [[gnu::always_inline]] const_iterator operator++(int) noexcept {
      const_iterator before = *this;
      cursor_.next();
      return before;
    }
    [[gnu::always_inline]] const_iterator& operator--() noexcept {
      cursor_.prev();
      return *this;
    }
    [[gnu::always_inline]] const_iterator operator--(int) noexcept {
      const_iterator before = *this;
      cursor_.prev();
      return before;
    }

    friend bool operator==(const const_iterator& a, const const_iterator& b) noexcept {
      return a.cursor_ == b.cursor_;
    }

   private:
    friend class detail::reverse_iterator<const_iterator>;
    explicit const_iterator(cursor_type cursor) noexcept : cursor_(std::move(cursor)) {}

    cursor_type cursor_;
  };

  using iterator = const_iterator;
  using reverse_iterator = detail::reverse_iterator<const_iterator>;
  using const_reverse_iterator = reverse_iterator;

  // --- Construction ---

  bbtree_set() noexcept = default;
  explicit bbtree_set(const Allocator& allocator) noexcept : owner(allocator) {}
  template <std::input_iterator It>
  bbtree_set(It first, It last, const Allocator& allocator = Allocator()) : owner(allocator) {
    insert(first, last);
  }
  bbtree_set(std::initializer_list<K> keys, const Allocator& allocator = Allocator()) : owner(allocator) {
    insert(keys.begin(), keys.end());
  }
  bbtree_set(const bbtree_set&) = default;
  bbtree_set(bbtree_set&&) noexcept = default;
  bbtree_set& operator=(const bbtree_set&) = default;
  bbtree_set& operator=(bbtree_set&&) noexcept = default;
  bbtree_set& operator=(std::initializer_list<K> keys) {
    clear();
    insert(keys.begin(), keys.end());
    return *this;
  }
  ~bbtree_set() = default;

  allocator_type get_allocator() const noexcept { return allocator_; }
  key_compare key_comp() const noexcept { return key_compare(); }
  value_compare value_comp() const noexcept { return value_compare(); }

  // --- Iterators ---

  const_iterator begin() const noexcept { return const_iterator(cursor_type::first(&tree_)); }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator end() const noexcept { return const_iterator(cursor_type(&tree_)); }
  const_iterator cend() const noexcept { return end(); }
  reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
  reverse_iterator crbegin() const noexcept { return rbegin(); }
  reverse_iterator rend() const noexcept { return reverse_iterator::at(end()); }
  reverse_iterator crend() const noexcept { return rend(); }

  // Runs `visit(key)` on every key in order, a leaf at a time: faster than
  // iterators (P16).
  template <class Visit>
  void for_each(Visit&& visit) const {
    auto key_only = [&](const K& key, detail::set_value&) { visit(key); };
    detail::in_order_walk<types>(tree_).run(key_only);
  }

  // `fold(accumulator, key)` over every key in order, from `init`, returning
  // the last accumulator: Rust's `iter().fold` (P16).
  template <class A, class Fold>
  A fold(A init, Fold&& fold) const {
    auto key_only = [&fold](A accumulator, const K& key, detail::set_value&) {
      return fold(std::move(accumulator), key);
    };
    return detail::in_order_walk<types>(tree_).fold(std::move(init), key_only);
  }

  // --- Capacity ---

  [[nodiscard]] bool empty() const noexcept { return length_ == 0; }
  size_type size() const noexcept { return length_; }
  size_type max_size() const noexcept { return static_cast<size_type>(PTRDIFF_MAX); }

  // --- Lookup ---

  const_iterator find(const K& key) const noexcept { return found_or_end(tree_.find(key)); }
  template <class Q>
    requires transparent<Q>
  const_iterator find(const Q& key) const noexcept {
    return found_or_end(tree_.find(key));
  }
  bool contains(const K& key) const noexcept { return tree_.find(key).has_value(); }
  template <class Q>
    requires transparent<Q>
  bool contains(const Q& key) const noexcept {
    return tree_.find(key).has_value();
  }
  size_type count(const K& key) const noexcept { return contains(key) ? 1 : 0; }

  const_iterator lower_bound(const K& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::before));
  }
  const_iterator upper_bound(const K& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::after));
  }
  template <class Q>
    requires transparent<Q>
  const_iterator lower_bound(const Q& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::before));
  }
  template <class Q>
    requires transparent<Q>
  const_iterator upper_bound(const Q& key) const noexcept {
    return const_iterator(cursor_type::beside(&tree_, key, detail::key_side::after));
  }
  std::pair<const_iterator, const_iterator> equal_range(const K& key) const noexcept {
    return {lower_bound(key), upper_bound(key)};
  }

  // --- Batches (D53) ---

  // Whether the set holds each of `keys`, in input order, looked up
  // together.
  template <class Q, std::size_t N>
    requires(std::is_same_v<Q, K> || transparent<Q>)
  std::array<bool, N> contains_many(const std::array<Q, N>& keys) const noexcept {
    std::array<const Q*, N> pointers;
    for (std::size_t index = 0; index < N; ++index) {
      pointers[index] = &keys[index];
    }
    std::array<bool, N> held;
    contains_many(std::span<const Q* const>(pointers), std::span<bool>(held));
    return held;
  }

  template <class Q>
    requires(std::is_same_v<Q, K> || transparent<Q>)
  void contains_many(std::span<const Q* const> keys, std::span<bool> held) const noexcept {
    std::optional<detail::found_entry<types>> found[detail::batch_lookup<types>::max_lanes];
    for (std::size_t start = 0; start < keys.size(); start += std::size(found)) {
      const std::size_t count = std::min(std::size(found), keys.size() - start);
      detail::batch_lookup<types>::find_many(
          tree_, length_, keys.subspan(start, count),
          std::span<std::optional<detail::found_entry<types>>>(found, count));
      for (std::size_t index = 0; index < count; ++index) {
        held[start + index] = found[index].has_value();
      }
    }
  }

  // Inserts each key in input order, their paths fetched together first;
  // returns whether each was new.
  template <std::size_t N>
  std::array<bool, N> insert_many(std::array<K, N>&& keys) {
    std::array<const K*, N> pointers;
    for (std::size_t index = 0; index < N; ++index) {
      pointers[index] = &keys[index];
    }
    std::array<bool, N> inserted;
    contains_many(std::span<const K* const>(pointers), std::span<bool>(inserted));
    for (std::size_t index = 0; index < N; ++index) {
      inserted[index] = insert(std::move(keys[index])).second;
    }
    return inserted;
  }

  const K* first() const noexcept {
    const auto found = tree_.first();
    return found ? found->key() : nullptr;
  }
  const K* last() const noexcept {
    const auto found = tree_.last();
    return found ? found->key() : nullptr;
  }

  // --- Inserting ---

  std::pair<const_iterator, bool> insert(const K& key) { return put(key); }
  std::pair<const_iterator, bool> insert(K&& key) { return put(std::move(key)); }
  const_iterator insert(const_iterator /*hint*/, const K& key) { return insert(key).first; }
  const_iterator insert(const_iterator /*hint*/, K&& key) { return insert(std::move(key)).first; }
  template <std::input_iterator It>
  void insert(It first, It last) {
    if (!empty()) {
      for (; first != last; ++first) {
        insert(K(*first));
      }
      return;
    }
    if constexpr (std::forward_iterator<It> && ranges_over_keys<It>) {
      auto out_of_order = [](const auto& a, const auto& b) { return !key_order::less(a, b); };
      if (std::adjacent_find(first, last, out_of_order) == last) {
        this->build_ascending(first, last, [](auto&& key) {
          return std::pair<K, detail::set_value>(K(std::forward<decltype(key)>(key)), detail::set_value{});
        });
        return;
      }
    }
    std::vector<std::pair<K, detail::set_value>> entries;
    bool ascending = true;
    for (; first != last; ++first) {
      entries.emplace_back(K(*first), detail::set_value{});
      if (entries.size() > 1 && !key_order::less(entries[entries.size() - 2].first, entries.back().first)) {
        ascending = false;
      }
    }
    this->build_from(entries, ascending);
  }
  void insert(std::initializer_list<K> keys) { insert(keys.begin(), keys.end()); }
  template <class... Args>
  std::pair<const_iterator, bool> emplace(Args&&... args) {
    return put(K(std::forward<Args>(args)...));
  }
  template <class... Args>
  const_iterator emplace_hint(const_iterator /*hint*/, Args&&... args) {
    return emplace(std::forward<Args>(args)...).first;
  }

  // --- Erasing ---

  size_type erase(const K& key) noexcept { return tree_.remove(key, pages(), length_) ? 1 : 0; }
  template <class Q>
    requires transparent<Q>
  size_type erase(const Q& key) noexcept {
    return tree_.remove(key, pages(), length_) ? 1 : 0;
  }
  const_iterator erase(const_iterator position) noexcept {
    cursor_type at = position.cursor_;
    detail::node_stack_mut<types> stack(tree_, at.attached_stack());
    stack.remove_and_go_to_next(pages(), length_);
    return const_iterator(cursor_type(&tree_, stack.stack()));
  }
  const_iterator erase(const_iterator first, const_iterator last) noexcept {
    std::size_t count = 0;
    for (const_iterator at = first; at != last; ++at) {
      ++count;
    }
    const_iterator at = first;
    for (; count > 0; --count) {
      at = erase(at);
    }
    return at;
  }

  std::optional<K> pop_first() noexcept {
    auto entry = tree_.pop_first(pages(), length_);
    return entry ? std::optional<K>(std::move(entry->first)) : std::nullopt;
  }
  std::optional<K> pop_last() noexcept {
    auto entry = tree_.pop_last(pages(), length_);
    return entry ? std::optional<K>(std::move(entry->first)) : std::nullopt;
  }
  // Removes and returns the key equal to `key`, if any (Rust's `take`).
  std::optional<K> take(const K& key) noexcept {
    auto entry = tree_.remove(key, pages(), length_);
    return entry ? std::optional<K>(std::move(entry->first)) : std::nullopt;
  }

  void clear() noexcept { this->clear_tree(); }
  void swap(bbtree_set& other) noexcept { this->swap_with(other); }
  friend void swap(bbtree_set& a, bbtree_set& b) noexcept { a.swap(b); }

  template <class Keep>
  size_type retain(Keep&& keep) {
    const size_type before = length_;
    const_iterator at = begin();
    while (at != end()) {
      if (keep(*at)) {
        ++at;
      } else {
        at = erase(at);
      }
    }
    return before - length_;
  }
  template <class Predicate>
  friend size_type erase_if(bbtree_set& set, Predicate predicate) {
    return set.retain([&](const K& key) { return !predicate(key); });
  }

  void append(bbtree_set& other) { this->append_from(other); }
  bbtree_set split_off(const K& key) {
    bbtree_set right(allocator_);
    this->split_off_into(key, right);
    return right;
  }

  // --- Set relations and algebra ---

  // Whether every key of this set is in `other`.
  bool is_subset(const bbtree_set& other) const {
    if (size() > other.size()) {
      return false;
    }
    if (looks_up_in(other)) {
      return std::all_of(begin(), end(), [&](const K& key) { return other.contains(key); });
    }
    return std::includes(other.begin(), other.end(), begin(), end(), Compare());
  }
  bool is_superset(const bbtree_set& other) const { return other.is_subset(*this); }

  // Whether the sets share no key.
  bool is_disjoint(const bbtree_set& other) const {
    if (looks_up_in(other)) {
      return std::none_of(begin(), end(), [&](const K& key) { return other.contains(key); });
    }
    if (other.looks_up_in(*this)) {
      return other.is_disjoint(*this);
    }
    const_iterator a = begin();
    const_iterator b = other.begin();
    while (a != end() && b != other.end()) {
      const auto ordering = key_order::cmp(*a, *b);
      if (ordering == 0) {
        return false;
      }
      ordering < 0 ? ++a : ++b;
    }
    return true;
  }

  friend bbtree_set operator|(const bbtree_set& a, const bbtree_set& b) {
    return a.merged(b, [](bool in_a, bool in_b) { return in_a || in_b; });
  }
  friend bbtree_set operator&(const bbtree_set& a, const bbtree_set& b) {
    if (a.looks_up_in(b)) {
      return a.filtered(a.allocator_, [&](const K& key) { return b.contains(key); });
    }
    if (b.looks_up_in(a)) {
      return b.filtered(a.allocator_, [&](const K& key) { return a.contains(key); });
    }
    return a.merged(b, [](bool in_a, bool in_b) { return in_a && in_b; });
  }
  friend bbtree_set operator-(const bbtree_set& a, const bbtree_set& b) {
    if (a.looks_up_in(b)) {
      return a.filtered(a.allocator_, [&](const K& key) { return !b.contains(key); });
    }
    return a.merged(b, [](bool in_a, bool in_b) { return in_a && !in_b; });
  }
  friend bbtree_set operator^(const bbtree_set& a, const bbtree_set& b) {
    return a.merged(b, [](bool in_a, bool in_b) { return in_a != in_b; });
  }

  // --- Comparison ---

  friend bool operator==(const bbtree_set& a, const bbtree_set& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), b.end());
  }
  friend auto operator<=>(const bbtree_set& a, const bbtree_set& b)
    requires std::three_way_comparable<K>
  {
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
  }

  void check_invariants() const { tree_.check_invariants(length_); }

 private:
  // Whether a range's elements are keys `K`. Only then does the set's order,
  // applied to the range as it is, say whether it ascends: other elements
  // convert to keys first, since a transparent comparator would compare them
  // as what they are (C strings as pointers).
  template <class It>
  static constexpr bool ranges_over_keys = std::same_as<std::remove_cvref_t<std::iter_reference_t<It>>, K>;

  // Whether a set operation should look this set's keys up in `other` one
  // by one, rather than walk both: when this set is much smaller.
  bool looks_up_in(const bbtree_set& other) const noexcept { return size() * 16 < other.size(); }

  const_iterator found_or_end(std::optional<detail::found_entry<types>> found) const noexcept {
    return found ? const_iterator(cursor_type(&tree_, *found)) : end();
  }

  template <class KeyRef>
  std::pair<const_iterator, bool> put(KeyRef&& key) {
    const auto inserted = tree_.template insert<detail::existing_value::kept>(
        std::forward<KeyRef>(key), [] { return detail::set_value{}; }, pages(), length_);
    return {const_iterator(cursor_type(&tree_, inserted.entry)), inserted.inserted};
  }

  // The keys of this set that `keep`, built in order on `allocator`: the
  // left operand's, whichever set an operator walks.
  template <class Keep>
  bbtree_set filtered(const Allocator& allocator, Keep keep) const {
    bbtree_set result(allocator);
    detail::tree_builder<types, Allocator> builder(result.pages());
    for (const K& key : *this) {
      if (keep(key)) {
        builder.push(K(key), detail::set_value{});
      }
    }
    result.adopt(std::move(builder).finish());
    return result;
  }

  // The keys in this set or `other` for which `keep(in_this, in_other)`, by a
  // walk over both in order, built as it goes.
  template <class Keep>
  bbtree_set merged(const bbtree_set& other, Keep keep) const {
    bbtree_set result(allocator_);
    detail::tree_builder<types, Allocator> builder(result.pages());
    const_iterator a = begin();
    const_iterator b = other.begin();
    while (a != end() || b != other.end()) {
      std::weak_ordering ordering = std::weak_ordering::equivalent;
      if (a == end()) {
        ordering = std::weak_ordering::greater;
      } else if (b == other.end()) {
        ordering = std::weak_ordering::less;
      } else {
        ordering = key_order::cmp(*a, *b);
      }
      const bool in_a = ordering <= 0;
      const bool in_b = ordering >= 0;
      if (keep(in_a, in_b)) {
        builder.push(K(in_a ? *a : *b), detail::set_value{});
      }
      if (in_a) {
        ++a;
      }
      if (in_b) {
        ++b;
      }
    }
    result.adopt(std::move(builder).finish());
    return result;
  }
};

}  // namespace unsync
}  // namespace bbtree
