// What a map and a set share: the tree, its length and the allocator, and the
// operations on whole trees that don't depend on what an entry looks like to
// the user: bulk builds (D25), merging appends, cuts, copies and teardown.
// `unsync::bbtree_map` and `unsync::bbtree_set` derive from it.
#pragma once

#include <bbtree/detail/tree/batch.hpp>
#include <bbtree/detail/tree/cursor.hpp>
#include <bbtree/detail/tree/tree.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bbtree::detail {

template <class T, class Allocator>
class tree_owner {
 protected:
  using K = typename T::key_type;
  using V = typename T::mapped_type;
  using tree_type = tree<T>;
  using cursor_type = cursor<T>;
  using pages_type = page_source<Allocator>;
  using order = typename T::order;

  tree_owner() noexcept = default;
  explicit tree_owner(const Allocator& allocator) noexcept : allocator_(allocator) {}
  tree_owner(const tree_owner& other)
      : allocator_(other.allocator_), tree_(other.tree_.clone(pages())), length_(other.length_) {}
  tree_owner(tree_owner&& other) noexcept
      : allocator_(other.allocator_),
        tree_(std::move(other.tree_)),
        length_(std::exchange(other.length_, 0)) {}
  tree_owner& operator=(const tree_owner& other) {
    if (this != &other) {
      tree_owner copy(other);
      swap_with(copy);
    }
    return *this;
  }
  tree_owner& operator=(tree_owner&& other) noexcept {
    if (this != &other) {
      clear_tree();
      allocator_ = other.allocator_;
      tree_ = std::move(other.tree_);
      length_ = std::exchange(other.length_, 0);
    }
    return *this;
  }
  ~tree_owner() { tree_.destroy(pages()); }

  pages_type pages() const noexcept { return pages_type(allocator_); }

  void clear_tree() noexcept {
    tree_.destroy(pages());
    length_ = 0;
  }

  void swap_with(tree_owner& other) noexcept {
    std::swap(allocator_, other.allocator_);
    std::swap(tree_, other.tree_);
    std::swap(length_, other.length_);
  }

  // Takes `built`'s tree in place of this one, which holds no entry but may
  // hold a root page: a tree keeps its root page when its last entry goes,
  // and gets one before an insert whose value then throws.
  void adopt(std::pair<tree_type, std::size_t>&& built) noexcept {
    clear_tree();
    tree_ = std::move(built.first);
    length_ = built.second;
  }

  // Fetches the paths of the `N` keys `key_at(index)` together, before they
  // are inserted one by one in that order: one key's misses then overlap the
  // others' (D53). Only where that pays, as the Rust crate's `insert_many`
  // decides: not in a tree below the batches' cutoff, whose nodes are in
  // cache, where it would be a second search of every key; and not if the
  // first key appends to the remembered leaf, as ascending keys do, which
  // descend through no node.
  template <std::size_t N, class KeyAt>
  void fetch_paths_for_inserts(KeyAt key_at) const noexcept {
    using lookup = batch_lookup<T>;
    if (N < 2 || !lookup::batches(length_) || tree_.appends_to_rightmost(key_at(0))) {
      return;
    }
    std::array<const K*, N> keys;
    for (std::size_t index = 0; index < N; ++index) {
      keys[index] = &key_at(index);
    }
    sought_entry<T> ignored[lookup::max_lanes];
    for (std::size_t start = 0; start < N; start += lookup::max_lanes) {
      const std::size_t count = std::min(lookup::max_lanes, N - start);
      lookup::find_many(tree_, length_, std::span<const K* const>(keys).subspan(start, count),
                        std::span<sought_entry<T>>(ignored, count));
    }
  }

  // Replaces an empty tree with one built straight from [first, last), whose
  // keys strictly ascend, `entry_of(*it)` making each entry: no copy of the
  // input, as Rust's in-place collect makes none (16% off a 1M u64 build).
  template <std::forward_iterator It, class EntryOf>
  void build_ascending(It first, It last, EntryOf entry_of) {
    tree_builder<T, Allocator> builder(pages());
    builder.extend(first, last, entry_of);
    adopt(std::move(builder).finish());
  }

  // Replaces an empty tree with one built from `entries`: sorted unless
  // `ascending`, keeping the first of equal keys, as std's containers do.
  void build_from(std::vector<std::pair<K, V>>& entries, bool ascending) {
    if (!ascending) {
      std::stable_sort(entries.begin(), entries.end(),
                       [](const auto& a, const auto& b) { return order::less(a.first, b.first); });
      auto equal_keys = [](const auto& a, const auto& b) { return !order::less(a.first, b.first); };
      entries.erase(std::unique(entries.begin(), entries.end(), equal_keys), entries.end());
    }
    tree_builder<T, Allocator> builder(pages());
    builder.extend(entries.begin(), entries.end(), [](std::pair<K, V>& entry) { return std::move(entry); });
    adopt(std::move(builder).finish());
  }

  // Moves every entry of `other` in, `other`'s winning at an equal key; both
  // trees' entries move in order into one bulk build (D25). The build's pages
  // come first, as many as it takes if no keys are equal: a page refused
  // throws `alloc_error` before an entry has moved, with both trees as they
  // were.
  void append_from(tree_owner& other) {
    if (other.length_ == 0) {
      return;
    }
    if (length_ == 0) {
      swap_with(other);
      return;
    }
    using prepaid_pages = prepaid_build_pages<T, Allocator>;
    const pages_type own_pages = pages();
    prepaid_pages prepaid(own_pages, length_ + other.length_);
    const typename prepaid_pages::allocator prepaid_allocator(prepaid);
    tree_builder<T, typename prepaid_pages::allocator> builder{
        page_source<typename prepaid_pages::allocator>(prepaid_allocator)};
    cursor_type mine(&tree_, node_end::first);
    cursor_type theirs(&other.tree_, node_end::first);
    auto take = [&](cursor_type& from) {
      builder.push(std::move(*from.key()), std::move(*from.value()));
      from.next();
    };
    while (!mine.is_end() && !theirs.is_end()) {
      const auto ordering = order::cmp(*mine.key(), *theirs.key());
      if (ordering < 0) {
        take(mine);
      } else if (ordering > 0) {
        take(theirs);
      } else {
        mine.next();
        take(theirs);
      }
    }
    while (!mine.is_end()) {
      take(mine);
    }
    while (!theirs.is_end()) {
      take(theirs);
    }
    // The moved-from shells are destroyed with their trees.
    other.clear_tree();
    adopt(std::move(builder).finish());
  }

  // Cuts off the entries from `key` on into `upper`, in O(log n).
  template <class Q>
  void split_off_into(const Q& key, tree_owner& upper) {
    auto cut = tree_.split_off_at(key, key_side::before, pages(), length_);
    upper.adopt(std::move(cut));
  }

  [[no_unique_address]] Allocator allocator_;
  tree_type tree_;
  std::size_t length_ = 0;
};

}  // namespace bbtree::detail
