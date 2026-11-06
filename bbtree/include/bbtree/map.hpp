// `bbtree::bbtree_map`: the ordered map that threads share (D4, D54), the
// unsync map's tree shared through optimistic lock coupling
// (09-concurrency.md).
//
// Closures instead of handles: an operation that exposes a key or value runs
// the caller's closure while the entry's leaf is latched, and returns what
// the closure returned, or hands out a copy. No reference, guard or latch
// outlives a call. There is no `size()`: a shared counter would cost every
// insert an atomic on one line all threads write.
//
//   bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
//   map.insert(1, 10);                                   // any thread
//   map.get_copied(1);                                   // std::optional{10}
//   map.update(1, [](const auto&, auto& value) { ++value; });
//   map.read(1, [](const auto&, const auto& value) { return value * 2; });
//
// Methods marked "owner" need the map to themselves, as the Rust crate's
// `&mut self` methods do: no other thread may use the map meanwhile.
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/detail/latched/changes.hpp>
#include <bbtree/detail/latched/latched_tree.hpp>
#include <bbtree/detail/latched/scan.hpp>
#include <bbtree/detail/tree/cursor.hpp>
#include <bbtree/node_config.hpp>

#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>

namespace bbtree {

template <class K, class V, class Compare = std::less<K>, page_allocator Allocator = global_allocator,
          class Strategy = auto_inline_keys>
class bbtree_map {
  using types = detail::tree_types<K, V, Compare, latched<Strategy>>;
  using tree_type = detail::latched_tree<types>;
  using pages_type = detail::page_source<Allocator>;

  template <class Q>
  static constexpr bool lookup_type = std::is_same_v<Q, K> || requires { typename Compare::is_transparent; };

 public:
  using key_type = K;
  using mapped_type = V;
  using key_compare = Compare;
  using allocator_type = Allocator;

  bbtree_map() noexcept = default;
  explicit bbtree_map(const Allocator& allocator) noexcept : allocator_(allocator) {}
  bbtree_map(const bbtree_map&) = delete;
  bbtree_map& operator=(const bbtree_map&) = delete;
  ~bbtree_map() { tree_.destroy(pages()); }

  // --- Lookups, from any thread ---

  // Runs `f(key, value)` on the entry at `key` while its leaf is latched
  // shared; returns what `f` returned, or nothing (for an `f` that returns
  // nothing: whether the entry was there).
  template <class Q, class F>
    requires lookup_type<Q>
  auto read(const Q& key, F&& f) const {
    return tree_.read(key, std::forward<F>(f));
  }

  // A copy of the value at `key`: read without a latch where the leaf
  // settles the key (D57); trivially copyable values only.
  template <class Q>
    requires(lookup_type<Q> && std::is_trivially_copyable_v<V>)
  std::optional<V> get_copied(const Q& key) const {
    return tree_.get_copied(key);
  }

  // A copy of the value at `key`, made with the leaf latched shared.
  template <class Q>
    requires lookup_type<Q>
  std::optional<V> get_cloned(const Q& key) const {
    return tree_.read(key, [](const K&, const V& value) { return V(value); });
  }

  template <class Q>
    requires lookup_type<Q> bool
  contains(const Q& key) const {
    return tree_.contains(key);
  }

  // Runs `visit(key, value)` on every entry in key order, each leaf latched
  // shared while its entries are visited, until `visit` returns false. Not a
  // snapshot: an entry present for the whole scan is visited exactly once.
  template <class Visit>
  void scan(Visit&& visit) const {
    detail::latched_scan<types>(tree_).template from<K>(nullptr, visit);
  }

  // The same, from the first entry not ordered before `start`.
  template <class Q, class Visit>
    requires lookup_type<Q>
  void scan_from(const Q& start, Visit&& visit) const {
    detail::latched_scan<types>(tree_).from(&start, visit);
  }

  // --- Changes, from any thread ---

  // Runs `f(key, value)` on the entry at `key` while its leaf is latched
  // exclusive; returns what `f` returned, or nothing.
  template <class Q, class F>
    requires lookup_type<Q>
  auto update(const Q& key, F&& f) {
    return tree_.update(key, std::forward<F>(f));
  }

  // Inserts `key` with `value` unless the map holds the key; returns whether
  // it did. Throws `alloc_error`, the map unchanged, if a page is refused.
  bool insert(K key, V value) {
    struct keep_existing {
      bool occupied(const K&, V&, V&&) noexcept { return false; }
      bool vacant(const K&, const V&) noexcept { return true; }
    } policy;
    return changes().insert(std::move(key), std::move(value), policy, pages());
  }

  // Inserts `key` with `value`, or replaces the value at `key`; returns the
  // value replaced, if any (the Rust crate's `insert`).
  std::optional<V> insert_or_assign(K key, V value) {
    struct replace {
      std::optional<V> occupied(const K&, V& stored, V&& with) noexcept {
        return std::exchange(stored, std::move(with));
      }
      std::optional<V> vacant(const K&, const V&) noexcept { return std::nullopt; }
    } policy;
    return changes().insert(std::move(key), std::move(value), policy, pages());
  }

  // Runs `f(key, stored, value)` on the entry at `key` with its leaf latched
  // exclusive, handing it the new value to merge; or inserts `key` and
  // `value` if there is none, returning nothing (for an `f` that returns
  // nothing: whether there was an entry).
  template <class F>
  auto update_or_insert(K key, V value, F&& f) {
    using result = detail::closure_result<std::invoke_result_t<F&, const K&, V&, V&&>>;
    struct merge {
      F& f;
      typename result::type occupied(const K& stored_key, V& stored, V&& with) {
        return result::of(f, stored_key, stored, std::move(with));
      }
      typename result::type vacant(const K&, const V&) noexcept { return result::none(); }
    } policy{f};
    return changes().insert(std::move(key), std::move(value), policy, pages());
  }

  // Runs `f(key, value)` on the entry at `key`, inserting `key` and `value`
  // first if there is none, with the leaf latched exclusive.
  template <class F>
  auto get_or_insert(K key, V value, F&& f) {
    struct read {
      F& f;
      auto occupied(const K& stored_key, V& stored, V&&) { return f(stored_key, std::as_const(stored)); }
      auto vacant(const K& new_key, const V& new_value) { return f(new_key, new_value); }
    } policy{f};
    return changes().insert(std::move(key), std::move(value), policy, pages());
  }

  // Removes and returns the entry at `key`, if any.
  template <class Q>
    requires lookup_type<Q>
  std::optional<std::pair<K, V>> remove(const Q& key) {
    auto always = [](const K&, const V&) { return true; };
    return changes().remove(key, always, pages());
  }

  // Removes and returns the entry at `key` if `condition(key, value)` holds
  // of it, its leaf latched exclusive.
  template <class Q, class Condition>
    requires lookup_type<Q>
  std::optional<std::pair<K, V>> remove_if(const Q& key, Condition&& condition) {
    return changes().remove(key, condition, pages());
  }

  template <class Q>
    requires lookup_type<Q> bool
  erase(const Q& key) {
    return remove(key).has_value();
  }

  // --- The owner's ---

  // The value at `key`, for the owner to change in place. Owner.
  template <class Q>
    requires lookup_type<Q>
  V* get_owned(const Q& key) {
    return tree_.change_owned([&](detail::tree<types>& whole) -> V* {
      const auto found = whole.find(key);
      return found ? found->value() : nullptr;
    });
  }

  // Drops every entry. Owner.
  void clear() noexcept { tree_.destroy(pages()); }

  // Runs `f(key, value)` on every entry in key order. Owner.
  template <class F>
  void for_each(F&& f) {
    tree_.change_owned([&](detail::tree<types>& whole) {
      for (auto at = detail::cursor<types>::first(&whole); !at.is_end(); at.next()) {
        f(std::as_const(*at.key()), *at.value());
      }
    });
  }

  // Inserts the entries of [first, last), each replacing an equal key's
  // value. Owner.
  template <std::input_iterator It>
  void extend(It first, It last) {
    std::size_t ignored_length = 0;
    tree_.change_owned([&](detail::tree<types>& whole) {
      for (; first != last; ++first) {
        const auto& entry = *first;
        whole.insert(
            K(entry.first), [&] { return V(entry.second); }, true, pages(), ignored_length);
      }
    });
  }

  // Panics, by `detail::invariant_violation`, unless the tree is well formed
  // and holds `length` entries. Owner; where `BBTREE_CHECKED` is set.
  void check_invariants(std::size_t length) { tree_.check_invariants(length); }

 private:
  pages_type pages() const noexcept { return pages_type(allocator_); }
  detail::latched_changes<types> changes() noexcept { return detail::latched_changes<types>(tree_); }

  [[no_unique_address]] Allocator allocator_;
  tree_type tree_;
};

}  // namespace bbtree
