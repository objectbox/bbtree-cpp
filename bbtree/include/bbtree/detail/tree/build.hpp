// Building a tree from entries in ascending key order, comparing no keys
// (D25): each leaf fills before the next starts beside it, so every node is
// full but along the right border, which `finish` tops up past the minimum
// from the full left siblings (`borders.hpp`). No node is split in half, as
// inserting the same entries one by one would do.
//
// Right-border nodes have no right fence, so their prefix is what every key
// shares (D50); a key that shares less shrinks that first. A full node closes
// once the next leaf starts after it and gets what its fences share as its
// prefix.
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <utility>

namespace bbtree::detail {

template <class T, class A>
class tree_builder {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

 public:
  explicit tree_builder(const page_source<A>& pages) noexcept : pages_(pages) {}
  tree_builder(const tree_builder&) = delete;
  tree_builder& operator=(const tree_builder&) = delete;
  ~tree_builder() { tree_.destroy(pages_); }

  // Appends an entry whose key is greater than every key before it.
  void push(K&& key, V&& value) {
    const std::size_t len = length_ > 0 ? border_leaf_.len() : leaf_node<T>::capacity;
    if (len < leaf_node<T>::capacity && tree_.shared().template is_shared_by<T>(key)) [[likely]] {
      border_leaf_.insert_fit(len,
                              prefixed_key<K&&>(std::move(key)).template window_at<T>(tree_.shared().len()),
                              std::move(value));
      ++length_;
      return;
    }
    push_past_run(std::move(key), std::move(value));
  }

  // Appends the entries `entry_of(*it)` of [first, last), whose keys ascend
  // past every key before them: `push` for each, with the run of a leaf (the
  // entries into the border leaf while it has room, under the prefix every
  // key shares) in a loop of its own, as Rust's `extend` loops. The loop
  // keeps the leaf, what every key shares and the count in locals, which
  // the stores into the leaf's page can't change: members would be read
  // back after each store (a quarter off a 1M u64 build).
  template <std::input_iterator It, class EntryOf>
  void extend(It first, It last, EntryOf entry_of) {
    leaf_node<T> leaf = border_leaf_;
    shared_prefix shared = tree_.shared();
    std::size_t len = length_ > 0 ? leaf.len() : leaf_node<T>::capacity;
    std::size_t run = 0;
    for (; first != last; ++first) {
      std::pair<K, V> entry = entry_of(*first);
      if (len < leaf_node<T>::capacity && shared.template is_shared_by<T>(entry.first)) [[likely]] {
        leaf.insert_fit(len, prefixed_key<K&&>(std::move(entry.first)).template window_at<T>(shared.len()),
                        std::move(entry.second));
        ++len;
        ++run;
        continue;
      }
      length_ += std::exchange(run, 0);
      push_past_run(std::move(entry.first), std::move(entry.second));
      leaf = border_leaf_;
      shared = tree_.shared();
      len = leaf.len();
    }
    length_ += run;
  }

  // The tree, with its right border fixed, and its length.
  std::pair<tree<T>, std::size_t> finish() && noexcept {
    if (tree_.has_root()) {
      tree_.template fix_border<node_end::last>(pages_);
      tree_.template refit_border_siblings<node_end::last>();
    }
    return {std::move(tree_), std::exchange(length_, 0)};
  }

 private:
  // The first entry, one past a full leaf, or one whose key shares less
  // than every key before it.
  [[gnu::noinline]] void push_past_run(K&& key, V&& value) {
    if (!tree_.has_root()) {
      tree_.make_root(pages_);
      border_leaf_ = leaf_node<T>(tree_.root());
    }
    const prefixed_key<K&&> prefixed = tree_.template shrink_shared_prefix_for<K&&>(std::move(key));
    const std::size_t shared_len = tree_.shared().len();
    if (border_leaf_.len() < leaf_node<T>::capacity) {
      border_leaf_.insert_fit(border_leaf_.len(), prefixed.template window_at<T>(shared_len),
                              std::move(value));
    } else {
      start_leaf(prefixed.template window_at<T>(shared_len), std::move(value));
    }
    ++length_;
  }

  // Starts a new leaf, the separator's head: the separator goes into the
  // lowest border node with room, under new border nodes for each full one,
  // raising the tree if every one is full. Its pages come first.
  void start_leaf(windowed_key<K>&& separator_key, V&& value) {
    reserved_pages<A> reserved(pages_);
    {
      std::size_t full = 0;
      std::size_t depth = 0;
      for (node_ptr<T> node = tree_.root(); !node.is_leaf(); node = internal_node<T>(node).edge(node.len())) {
        full = node.len() == internal_node<T>::capacity ? full + 1 : 0;
        ++depth;
      }
      const bool raises = full == depth;
      reserved.reserve(page_kind::leaf, T::leaf.page, depth == 0 ? 2 : 1);
      reserved.reserve(page_kind::internal, T::internal.page, full + (raises && depth > 0 ? 1 : 0));
    }
    const std::size_t shared_len = tree_.shared().len();
    const leaf_node<T> leaf =
        leaf_node<T>::create(fresh_page<T, page_role::leaf>::allocate(reserved), shared_len);
    separator<T> middle = leaf.put_separator_head(std::move(separator_key), std::move(value));
    node_ptr<T> child = leaf.node();
    node_stack_mut<T> stack(tree_);
    stack.start_at_root();
    stack.stack().descend_to_last_key();
    while (true) {
      const internal_node<T> node = stack.ascend_raising(reserved);
      const std::size_t len = node.len();
      if (len < internal_node<T>::capacity) {
        node.insert_fit(len, std::move(middle), child);
        break;
      }
      child =
          internal_node<T>::create(fresh_page<T, page_role::internal>::allocate(reserved), child, shared_len)
              .node();
    }
    border_leaf_ = leaf;
    // The nodes the new leaf closed fit their prefixes to their fences.
    const std::size_t edge = stack.index();
    stack.template refit_border_below<node_end::last>(edge);
    stack.refit_children_to_fences(edge, std::nullopt);
  }

  page_source<A> pages_;
  tree<T> tree_;
  leaf_node<T> border_leaf_;
  std::size_t length_ = 0;
};

// How many pages of each kind a `tree_builder` takes for a number of entries,
// the root page among its kind's: every leaf but the last is full, with one
// more entry as the head of the leaf after it, and so is every internal node
// but the last of its level; the top node lives in the root page.
struct build_page_counts {
  std::size_t leaf = 0;
  std::size_t internal = 0;

  template <class T>
  static constexpr build_page_counts of(std::size_t entries) noexcept {
    build_page_counts counts;
    if (entries == 0) {
      return counts;
    }
    (T::root_is_leaf_sized ? counts.leaf : counts.internal) = 1;
    std::size_t nodes = entries / (leaf_node<T>::capacity + 1) + 1;
    if (nodes == 1) {
      return counts;
    }
    counts.leaf += nodes;
    while (true) {
      nodes = (nodes + internal_node<T>::capacity) / (internal_node<T>::capacity + 1);
      if (nodes == 1) {
        return counts;
      }
      counts.internal += nodes;
    }
  }
};

// The pages of a bulk build, taken from a map's allocator before the build
// moves its first entry (D8). `append` merges two trees by moving their
// entries into one build: a page refused part of the way would leave both
// trees holding moved-from entries, so it takes every page the build can
// take first, and the build then can't fail. Pages wait linked through their
// first word, are handed out zeroed again, and take back what the build
// frees; what is left at the end goes back to the map's allocator.
template <class T, class A>
class prepaid_build_pages {
 public:
  // Takes the pages of a build of at most `entries` entries. Throws
  // `alloc_error`, with every page given back, if one is refused.
  prepaid_build_pages(const page_source<A>& source, std::size_t entries) : source_(source) {
    const build_page_counts counts = build_page_counts::of<T>(entries);
    try {
      for (std::size_t taken = 0; taken < counts.leaf; ++taken) {
        push(page_kind::leaf, source_.allocate(page_kind::leaf, T::leaf.page));
      }
      for (std::size_t taken = 0; taken < counts.internal; ++taken) {
        push(page_kind::internal, source_.allocate(page_kind::internal, T::internal.page));
      }
    } catch (const alloc_error&) {
      give_back(page_kind::leaf);
      give_back(page_kind::internal);
      throw;
    }
  }
  prepaid_build_pages(const prepaid_build_pages&) = delete;
  prepaid_build_pages& operator=(const prepaid_build_pages&) = delete;
  ~prepaid_build_pages() {
    give_back(page_kind::leaf);
    give_back(page_kind::internal);
  }

  // The page allocator a `tree_builder` builds on (`allocator.hpp`): each
  // kind's pages from the prepaid ones.
  class allocator {
   public:
    explicit allocator(prepaid_build_pages& pages) noexcept : pages_(&pages) {}

    void* allocate_leaf(page_layout /*layout*/) const noexcept { return pages_->pop(page_kind::leaf); }
    void* allocate_internal(page_layout /*layout*/) const noexcept {
      return pages_->pop(page_kind::internal);
    }
    void deallocate_leaf(void* page, page_layout /*layout*/) const noexcept {
      pages_->take_back(page_kind::leaf, page);
    }
    void deallocate_internal(void* page, page_layout /*layout*/) const noexcept {
      pages_->take_back(page_kind::internal, page);
    }
    // A build asks for its pages by kind only.
    void* allocate_zeroed(page_layout /*layout*/) const noexcept { return nullptr; }
    void deallocate(void* /*page*/, page_layout /*layout*/) const noexcept {}

   private:
    prepaid_build_pages* pages_;
  };

 private:
  static constexpr page_layout layout_of(page_kind kind) noexcept {
    return kind == page_kind::leaf ? T::leaf.page : T::internal.page;
  }
  void*& top_of(page_kind kind) noexcept { return kind == page_kind::leaf ? leaf_top_ : internal_top_; }

  void push(page_kind kind, void* page) noexcept {
    void*& top = top_of(kind);
    std::memcpy(page, &top, sizeof top);
    top = page;
  }

  void* pop(page_kind kind) noexcept {
    void*& top = top_of(kind);
    assert(top != nullptr && "build_page_counts covers every page a build takes");
    if (top == nullptr) {
      return source_.try_allocate(kind, layout_of(kind));
    }
    void* page = top;
    std::memcpy(&top, page, sizeof top);
    std::memset(page, 0, sizeof top);
    return page;
  }

  void take_back(page_kind kind, void* page) noexcept {
    std::memset(page, 0, layout_of(kind).size);
    push(kind, page);
  }

  void give_back(page_kind kind) noexcept {
    void*& top = top_of(kind);
    while (top != nullptr) {
      void* page = top;
      std::memcpy(&top, page, sizeof top);
      source_.deallocate(kind, page, layout_of(kind));
    }
  }

  const page_source<A>& source_;
  void* leaf_top_ = nullptr;
  void* internal_top_ = nullptr;
};

}  // namespace bbtree::detail
