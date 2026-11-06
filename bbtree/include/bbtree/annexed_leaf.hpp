// `bbtree::annexed_leaf`: a leaf of a shared map that an insert or a removal
// holds exclusively while it runs its caller's closure
// (`bbtree_map::update_or_insert_with_leaf`, `bbtree_map::remove_with_leaf`):
// the leaf's annex (D60, `leaf_annex.hpp`), and its keys. A handle, valid
// only during the closure.
//
// No other thread reads or changes the leaf's keys meanwhile, but for its
// head's: the head key is the separator left of the leaf (D49), which a
// thread holding the internal node it separates in reads, to refit that
// node's prefix, without holding the leaf. Code that changes what a key
// points to (as bbtree-store's keys point to their records) leaves the head
// key, and every byte its ordering reads, as they are.
#pragma once

#include <bbtree/detail/node/leaf.hpp>

#include <cassert>
#include <cstddef>
#include <type_traits>

namespace bbtree {

template <class T>
class annexed_leaf {
  using K = typename T::key_type;

 public:
  using annex_type = typename T::annex;

  // `leaf`, which the caller holds exclusively.
  explicit annexed_leaf(detail::leaf_node<T> leaf) noexcept : leaf_(leaf) {}

  // The leaf's annex.
  std::add_lvalue_reference_t<annex_type> annex() noexcept
    requires T::has_leaf_annex
  {
    return leaf_.annex();
  }

  // How many keys the leaf holds, its head's included (D17).
  std::size_t key_count() const noexcept { return leaf_.len() + (leaf_.has_head() ? 1 : 0); }

  // Whether the leaf has a head, `key(0)`: a separator, which threads holding
  // an internal node read without holding the leaf (top of the file). Every
  // leaf but the tree's first has one.
  bool has_head() const noexcept { return leaf_.has_head(); }

  // The leaf's keys in order, `index < key_count()`: its head's first, if it
  // has one, the separator's left of it (D49). They stay put while the leaf
  // is held.
  const K& key(std::size_t index) const noexcept {
    assert(index < key_count() && "key index out of bounds");
    if (leaf_.has_head()) {
      return index == 0 ? leaf_.head_key() : leaf_.key(index - 1);
    }
    return leaf_.key(index);
  }

  // Whether `key`, one of the leaf's keys, is its head.
  bool is_head(const K& key) const noexcept { return leaf_.has_head() && &leaf_.head_key() == &key; }

 private:
  detail::leaf_node<T> leaf_;
};

}  // namespace bbtree
