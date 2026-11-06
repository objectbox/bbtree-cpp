// The leaf annex (D60): data of the caller's that a shared map keeps in each
// of its leaves, right after the leaf's header, for the code that owns where
// the map's keys and values point, such as bbtree-store's chain of heap pages.
// The map keeps it with its leaf:
// - a new leaf gets a default-constructed annex;
// - a split gives the new leaf, right of the one that splits, a default
//   annex, then calls `left.split(right)` once the entries right of the split
//   moved there;
// - a merge calls `left.merge(std::move(right))` on the leaf that stays with
//   the one that goes;
// - dropping or clearing the map calls `std::move(annex).release(allocator)`
//   on each, with the map's allocator.
// Entries also move between neighbouring leaves without a call, as a removal
// rebalances them.
//
// Only a shared map (`bbtree::bbtree_map`) keeps annexes: an unsync map or a
// file map with one fails to compile. A map gives its strategy an annex with
// `with_leaf_annex`:
//
//   bbtree::bbtree_map<K, V, std::less<K>, A, bbtree::with_leaf_annex<bbtree::auto_inline_keys, my_annex>>
//
// and hands the annex to the closures of `update_or_insert_with_leaf` and
// `remove_with_leaf`, which run while their leaf is held exclusively
// (`annexed_leaf.hpp`).
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/relocation.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace bbtree {

// What a leaf annex is: default-constructible, moved with its leaf's bytes
// (the node code relocates leaves by copying their pages, P6), and told of
// its leaf's splits and merges:
//
//   void split(L& right) noexcept;  // its leaf split into itself and
//                                   // `right`'s, which took the entries
//                                   // right of the split
//   void merge(L&& gone) noexcept;  // `gone`'s leaf merged into its, and goes
//
// None of its members may throw: they run in the middle of a split or a
// merge.
template <class L>
concept leaf_annex = std::is_nothrow_default_constructible_v<L> && std::is_nothrow_destructible_v<L> &&
                     is_trivially_relocatable_v<L> && requires(L& annex, L& right, L&& gone) {
                                                        { annex.split(right) } noexcept;
                                                        { annex.merge(std::move(gone)) } noexcept;
                                                      };

// An annex that a map of allocator `A` releases, as its leaf goes with the
// map, giving what it holds back to the map's allocator:
//
//   void release(const A& allocator) && noexcept;
template <class L, class A>
concept leaf_annex_for = leaf_annex<L> && page_allocator<A> && requires(L&& annex, const A& allocator) {
                                                                 {
                                                                   std::move(annex).release(allocator)
                                                                 } noexcept;
                                                               };

// The strategy whose inline keys and node config are strategy `S`'s, and
// whose leaves keep an annex `L`: for a shared map.
template <class S, leaf_annex L>
struct with_leaf_annex {};

}  // namespace bbtree
