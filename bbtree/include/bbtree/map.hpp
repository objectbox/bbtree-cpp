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
#include <bbtree/detail/latched/latched_tree.hpp>
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

  // --- Changes, from any thread ---

  // Runs `f(key, value)` on the entry at `key` while its leaf is latched
  // exclusive; returns what `f` returned, or nothing.
  template <class Q, class F>
    requires lookup_type<Q>
  auto update(const Q& key, F&& f) {
    return tree_.update(key, std::forward<F>(f));
  }

  // --- The owner's ---

  // Inserts the entries of [first, last), each replacing an equal key's
  // value. Owner.
  template <std::input_iterator It>
  void extend(It first, It last) {
    std::size_t ignored_length = 0;
    tree_.change_held([&](detail::tree<types>& whole) {
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

  [[no_unique_address]] Allocator allocator_;
  tree_type tree_;
};

}  // namespace bbtree
