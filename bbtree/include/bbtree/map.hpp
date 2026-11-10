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
//
// A map whose strategy is `with_leaf_annex<S, L>` keeps an `L` in each leaf
// (D60, `leaf_annex.hpp`), which `update_or_insert_with_leaf` and
// `remove_with_leaf` hand their closures with the leaf they hold.
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/annexed_leaf.hpp>
#include <bbtree/detail/latched/changes.hpp>
#include <bbtree/detail/latched/latched_tree.hpp>
#include <bbtree/detail/latched/scan.hpp>
#include <bbtree/detail/tree/walk.hpp>
#include <bbtree/node_config.hpp>

#include <concepts>
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

  using leaf = detail::leaf_node<types>;

  // Lookups take `const K&`, which converts as std::map's do, and other key
  // types with a transparent comparator.
  template <class Q>
  static constexpr bool lookup_type = !std::is_same_v<Q, K> && requires { typename Compare::is_transparent; };

  static_assert(!types::has_leaf_annex || leaf_annex_for<typename types::annex, Allocator>,
                "a map's leaf annex releases to the map's allocator (D60)");

 public:
  using key_type = K;
  using mapped_type = V;
  using key_compare = Compare;
  using allocator_type = Allocator;
  // A leaf an insert or a removal holds while it runs its caller's closure,
  // with its annex (D60).
  using annexed_leaf_type = annexed_leaf<types>;

  bbtree_map() noexcept = default;
  explicit bbtree_map(const Allocator& allocator) noexcept : allocator_(allocator) {}
  bbtree_map(const bbtree_map&) = delete;
  bbtree_map& operator=(const bbtree_map&) = delete;
  // Moves and swaps are the owner's: no other thread uses either map. A
  // moved-from map is empty.
  bbtree_map(bbtree_map&& other) noexcept : allocator_(other.allocator_) { tree_.swap(other.tree_); }
  bbtree_map& operator=(bbtree_map&& other) noexcept {
    bbtree_map taken(std::move(other));
    swap(taken);
    return *this;
  }
  ~bbtree_map() { tree_.destroy(pages()); }

  void swap(bbtree_map& other) noexcept {
    using std::swap;
    swap(allocator_, other.allocator_);
    tree_.swap(other.tree_);
  }
  friend void swap(bbtree_map& a, bbtree_map& b) noexcept { a.swap(b); }

  // --- Lookups, from any thread ---

  // Runs `f(key, value)` on the entry at `key` while its leaf is latched
  // shared; returns what `f` returned, or nothing (for an `f` that returns
  // nothing: whether the entry was there).
  template <class F>
  auto read(const K& key, F&& f) const {
    return tree_.read(key, std::forward<F>(f));
  }
  template <class Q, class F>
    requires lookup_type<Q>
  auto read(const Q& key, F&& f) const {
    return tree_.read(key, std::forward<F>(f));
  }

  // A copy of the value at `key`: read without a latch where the leaf
  // settles the key (D57); trivially copyable values only.
  std::optional<V> get_copied(const K& key) const
    requires std::is_trivially_copyable_v<V>
  {
    return tree_.get_copied(key);
  }
  template <class Q>
    requires(lookup_type<Q> && std::is_trivially_copyable_v<V>)
  std::optional<V> get_copied(const Q& key) const {
    return tree_.get_copied(key);
  }

  // A copy of the value at `key`, made with the leaf latched shared.
  std::optional<V> get_cloned(const K& key) const {
    return tree_.read(key, [](const K&, const V& value) { return V(value); });
  }
  template <class Q>
    requires lookup_type<Q>
  std::optional<V> get_cloned(const Q& key) const {
    return tree_.read(key, [](const K&, const V& value) { return V(value); });
  }

  bool contains(const K& key) const { return tree_.contains(key); }
  template <class Q>
    requires(lookup_type<Q>)
  bool contains(const Q& key) const {
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
  template <class Visit>
  void scan_from(const K& start, Visit&& visit) const {
    detail::latched_scan<types>(tree_).from(&start, visit);
  }
  template <class Q, class Visit>
    requires lookup_type<Q>
  void scan_from(const Q& start, Visit&& visit) const {
    detail::latched_scan<types>(tree_).from(&start, visit);
  }

  // `scan` in reverse key order: from the last entry back.
  template <class Visit>
  void scan_rev(Visit&& visit) const {
    detail::latched_scan<types>(tree_).template back_from<K>(nullptr, visit);
  }

  // The same, from the last entry not ordered after `end` back.
  template <class Visit>
  void scan_rev_from(const K& end, Visit&& visit) const {
    detail::latched_scan<types>(tree_).back_from(&end, visit);
  }
  template <class Q, class Visit>
    requires lookup_type<Q>
  void scan_rev_from(const Q& end, Visit&& visit) const {
    detail::latched_scan<types>(tree_).back_from(&end, visit);
  }

  // --- Changes, from any thread ---

  // Runs `f(key, value)` on the entry at `key` while its leaf is latched
  // exclusive; returns what `f` returned, or nothing.
  template <class F>
  auto update(const K& key, F&& f) {
    return tree_.update(key, std::forward<F>(f));
  }
  template <class Q, class F>
    requires lookup_type<Q>
  auto update(const Q& key, F&& f) {
    return tree_.update(key, std::forward<F>(f));
  }

  // Inserts `key` with `value` unless the map holds the key; returns whether
  // it did. Throws `alloc_error`, the map unchanged, if a page is refused.
  bool insert(K key, V value) {
    struct keep_existing {
      bool occupied(leaf, const K&, V&, V&&) noexcept { return false; }
      bool vacant(leaf, const K&, const V&) noexcept { return true; }
    } policy;
    return changes().insert(std::move(key), std::move(value), policy, pages());
  }

  // Inserts `key` with `value`, or replaces the value at `key`; returns the
  // value replaced, if any (the Rust crate's `insert`).
  std::optional<V> insert_or_assign(K key, V value) {
    struct replace {
      std::optional<V> occupied(leaf, const K&, V& stored, V&& with) noexcept {
        return std::exchange(stored, std::move(with));
      }
      std::optional<V> vacant(leaf, const K&, const V&) noexcept { return std::nullopt; }
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
      typename result::type occupied(leaf, const K& stored_key, V& stored, V&& with) {
        return result::of(f, stored_key, stored, std::move(with));
      }
      typename result::type vacant(leaf, const K&, const V&) noexcept { return result::none(); }
    } policy{f};
    return changes().insert(std::move(key), std::move(value), policy, pages());
  }

  // Runs `f(key, value)` on the entry at `key`, inserting `key` and `value`
  // first if there is none, with the leaf latched exclusive.
  template <class F>
  auto get_or_insert(K key, V value, F&& f) {
    struct read {
      F& f;
      auto occupied(leaf, const K& stored_key, V& stored, V&&) {
        return f(stored_key, std::as_const(stored));
      }
      auto vacant(leaf, const K& new_key, const V& new_value) { return f(new_key, new_value); }
    } policy{f};
    return changes().insert(std::move(key), std::move(value), policy, pages());
  }

  // Runs `occupied(leaf, key, value)` on the entry at `key`, its value
  // exclusive, or, if the map holds none, `vacant(leaf, key, value)` on `key`
  // and `value`, which go in right after; each with the leaf the entry is
  // in, or goes in (`annexed_leaf_type`, its annex included, D60), while no
  // other thread reads or changes that leaf. Returns what the closure
  // returned; both return the same type, not void.
  //
  // Throws `alloc_error`, the map unchanged, if the allocator refuses a node
  // page, which is taken before `vacant` runs. A closure that throws leaves
  // the map unchanged too. Closures that may lack a page of their own refuse
  // without an exception through `try_update_or_insert_with_leaf`.
  template <class Occupied, class Vacant>
  auto update_or_insert_with_leaf(K key, V value, Occupied&& occupied, Vacant&& vacant) {
    with_leaf<Occupied, Vacant, std::false_type> policy{occupied, vacant};
    return changes().insert(std::move(key), std::move(value), policy, pages());
  }

  // `update_or_insert_with_leaf` with closures that may refuse, for want of
  // a page: both return a `std::optional`, empty to refuse, having changed
  // nothing. Returns what the closure returned; empty, the map unchanged, if
  // it refused, and if the allocator refused a node page. No exception for
  // want of a page.
  template <class Occupied, class Vacant>
  auto try_update_or_insert_with_leaf(K key, V value, Occupied&& occupied, Vacant&& vacant) {
    with_leaf<Occupied, Vacant, std::true_type> policy{occupied, vacant};
    return changes().insert(std::move(key), std::move(value), policy, pages());
  }

  // Removes and returns the entry at `key`, if any.
  std::optional<std::pair<K, V>> remove(const K& key) {
    auto always = [](const K&, const V&) { return true; };
    auto nothing = [](leaf, const K&, const V&) noexcept {};
    return changes().remove(key, always, nothing, pages());
  }
  template <class Q>
    requires lookup_type<Q>
  std::optional<std::pair<K, V>> remove(const Q& key) {
    auto always = [](const K&, const V&) { return true; };
    auto nothing = [](leaf, const K&, const V&) noexcept {};
    return changes().remove(key, always, nothing, pages());
  }

  // Removes and returns the entry at `key` if `condition(key, value)` holds
  // of it, its leaf latched exclusive.
  template <class Condition>
  std::optional<std::pair<K, V>> remove_if(const K& key, Condition&& condition) {
    auto nothing = [](leaf, const K&, const V&) noexcept {};
    return changes().remove(key, condition, nothing, pages());
  }
  template <class Q, class Condition>
    requires lookup_type<Q>
  std::optional<std::pair<K, V>> remove_if(const Q& key, Condition&& condition) {
    auto nothing = [](leaf, const K&, const V&) noexcept {};
    return changes().remove(key, condition, nothing, pages());
  }

  // Removes and returns the entry at `key`, if any, the key as the map
  // stored it. `f(leaf, key, value)` runs once on it, with the leaf it is in
  // (D60), right before it comes out, while no other thread reads or changes
  // the leaf.
  template <class Q, class F>
    requires(std::is_same_v<Q, K> || lookup_type<Q>)
  std::optional<std::pair<K, V>> remove_with_leaf(const Q& key, F&& f) {
    auto always = [](const K&, const V&) { return true; };
    auto before_removal = [&](leaf held, const K& stored_key, const V& stored) {
      f(annexed_leaf_type(held), stored_key, stored);
    };
    return changes().remove(key, always, before_removal, pages());
  }

  bool erase(const K& key) { return remove(key).has_value(); }
  template <class Q>
    requires(lookup_type<Q>)
  bool erase(const Q& key) {
    return remove(key).has_value();
  }

  // --- The owner's ---

  // The value at `key`, for the owner to change in place. Owner.
  V* get_owned(const K& key) {
    return tree_.change_owned([&](detail::tree<types>& whole) -> V* {
      const auto found = whole.find(key);
      return found ? found->value() : nullptr;
    });
  }
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
    tree_.change_owned([&](detail::tree<types>& whole) { detail::in_order_walk<types>(whole).run(f); });
  }

  // Inserts the entries of [first, last), each replacing an equal key's
  // value. Owner.
  template <std::input_iterator It>
  void extend(It first, It last) {
    std::size_t ignored_length = 0;
    tree_.change_owned([&](detail::tree<types>& whole) {
      for (; first != last; ++first) {
        const auto& entry = *first;
        whole.template insert<detail::existing_value::assigned>(
            K(entry.first), [&] { return V(entry.second); }, pages(), ignored_length);
      }
    });
  }

  // Panics, by `detail::invariant_violation`, unless the tree is well formed
  // and holds `length` entries. Owner; where `BBTREE_CHECKED` is set.
#if defined(BBTREE_CHECKED)
  void check_invariants(std::size_t length) { tree_.check_invariants(length); }
#endif

 private:
  pages_type pages() const noexcept { return pages_type(allocator_); }
  detail::latched_changes<types> changes() noexcept { return detail::latched_changes<types>(tree_); }

  // The insert policy of the `*_with_leaf` entry points: the caller's
  // closures, each run with the handle of the leaf the map holds; `MayRefuse`
  // says whether they may refuse (`detail::refusing_insert_policy`).
  template <class Occupied, class Vacant, class MayRefuse>
  struct with_leaf {
    using may_refuse = MayRefuse;
    using result = std::invoke_result_t<Occupied&, annexed_leaf_type, const K&, V&>;
    static_assert(
        std::is_same_v<result, std::invoke_result_t<Vacant&, annexed_leaf_type, const K&, const V&>>,
        "both closures return the same type");
    static_assert(!std::is_void_v<result>, "the closures return a value");
    static_assert(
        !MayRefuse::value || requires {
          requires std::same_as<result, std::optional<typename result::value_type>>;
        }, "closures that may refuse return a std::optional, empty to refuse");

    Occupied& on_occupied;
    Vacant& on_vacant;

    result occupied(leaf held, const K& stored_key, V& stored, V&&) {
      return on_occupied(annexed_leaf_type(held), stored_key, stored);
    }
    result vacant(leaf held, const K& new_key, const V& new_value) {
      return on_vacant(annexed_leaf_type(held), new_key, new_value);
    }
  };

  [[no_unique_address]] Allocator allocator_;
  tree_type tree_;
};

}  // namespace bbtree
