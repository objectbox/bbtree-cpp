// Type-stable pages (09-concurrency.md): a latched tree's node pages stay node
// pages of their kind for as long as threads share the tree.
//
// An optimistic reader follows pointers it read without a latch and checks
// them only after, so a node a merge emptied may still be read. Its page goes
// on the tree's free list for its kind, comes back only as a node of that
// kind, and keeps its latch word, whose version only grows: a reader that
// reaches it fails its check. Pages go back to the allocator when the map is
// dropped.
//
// `node_pages` is one operation's page source: it takes the pages the
// operation will allocate before anything changes (free pages first), holds
// each page it hands out latched until the operation is done, and puts the
// pages the operation frees on the free lists only then.
#pragma once

#include <bbtree/detail/latched/latch.hpp>
#include <bbtree/detail/node/page.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>

namespace bbtree::detail {

// A stack of free pages of one kind, linked through each page's first
// inline-key slot, behind a spin lock (pops and pushes are rare).
class free_list {
 public:
  static constexpr std::size_t link_offset = latched_node_header_layout::size;

  void push(std::byte* page) noexcept {
    with_locked([&] {
      std::byte* top = top_.load(std::memory_order_relaxed);
      std::memcpy(page + link_offset, &top, sizeof top);
      top_.store(page, std::memory_order_relaxed);
    });
  }

  // Exchanges the two lists' pages. Owner: no thread pushes or pops.
  void swap(free_list& other) noexcept {
    std::byte* const top = top_.load(std::memory_order_relaxed);
    top_.store(other.top_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    other.top_.store(top, std::memory_order_relaxed);
  }

  std::byte* pop() noexcept {
    if (top_.load(std::memory_order_relaxed) == nullptr) {
      return nullptr;
    }
    std::byte* page = nullptr;
    with_locked([&] {
      page = top_.load(std::memory_order_relaxed);
      if (page != nullptr) {
        std::byte* next;
        std::memcpy(&next, page + link_offset, sizeof next);
        top_.store(next, std::memory_order_relaxed);
      }
    });
    return page;
  }

 private:
  template <class F>
  void with_locked(F&& critical) noexcept {
    backoff wait;
    bool expected = false;
    while (!locked_.compare_exchange_weak(expected, true, std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
      expected = false;
      wait.wait();
    }
    critical();
    locked_.store(false, std::memory_order_release);
  }

  std::atomic<bool> locked_{false};
  std::atomic<std::byte*> top_{nullptr};
};

template <class T>
class free_lists {
 public:
  free_list& of(page_kind kind) noexcept { return kind == page_kind::leaf ? leaves_ : internals_; }

  // Owner: no thread pushes or pops.
  void swap(free_lists& other) noexcept {
    leaves_.swap(other.leaves_);
    internals_.swap(other.internals_);
  }

  // Gives every free page back to the allocator: when the map is dropped.
  template <class A>
  void deallocate_all(const page_source<A>& source) noexcept {
    for (const page_kind kind : {page_kind::leaf, page_kind::internal}) {
      const page_layout layout = kind == page_kind::leaf ? T::leaf.page : T::internal.page;
      while (std::byte* page = of(kind).pop()) {
        source.deallocate(kind, page, layout);
      }
    }
  }

 private:
  free_list leaves_;
  free_list internals_;
};

template <class T, class A>
class node_pages {
 public:
  node_pages(free_lists<T>& free, const page_source<A>& source) noexcept : free_(free), source_(source) {}
  node_pages(const node_pages&) = delete;
  node_pages& operator=(const node_pages&) = delete;

  // Lets go of every page the operation allocated, at its next version, and
  // frees the pages it freed, now that its nodes are let go.
  ~node_pages() {
    unreserve();
    for (std::size_t slot = 0; slot < allocated_count_; ++slot) {
      node_latch(allocated_[slot].page).unlock_exclusive(allocated_[slot].version);
    }
    for (std::size_t slot = 0; slot < freed_count_; ++slot) {
      free_.of(freed_[slot].kind).push(freed_[slot].page);
    }
  }

  // Takes `count` pages of `kind`, free ones first; false, giving back what
  // it took, if the allocator refuses one.
  bool reserve(page_kind kind, std::size_t count) noexcept {
    for (std::size_t taken = 0; taken < count; ++taken) {
      std::byte* page = free_.of(kind).pop();
      if (page == nullptr) {
        page = static_cast<std::byte*>(source_.try_allocate(kind, layout_of(kind)));
      }
      if (page == nullptr) {
        unreserve();
        return false;
      }
      assert(reserved_count_ < max_pages);
      reserved_[reserved_count_++] = {page, kind};
    }
    return true;
  }

  // Gives back the pages reserved and not taken.
  void unreserve() noexcept {
    for (std::size_t slot = 0; slot < reserved_count_; ++slot) {
      free_.of(reserved_[slot].kind).push(reserved_[slot].page);
    }
    reserved_count_ = 0;
  }

  // Whether the operation freed `page`: for appends, whose last leaf a merge
  // may have freed.
  bool has_freed(const std::byte* page) const noexcept {
    for (std::size_t slot = 0; slot < freed_count_; ++slot) {
      if (freed_[slot].page == page) {
        return true;
      }
    }
    return false;
  }

  // The page source interface the tree's operations take pages through.
  void* allocate(page_kind kind, [[maybe_unused]] page_layout layout) const noexcept {
    assert(layout == layout_of(kind) && "a kind's pages have one layout");
    std::byte* page = nullptr;
    for (std::size_t slot = reserved_count_; slot-- > 0;) {
      if (reserved_[slot].kind == kind) {
        page = reserved_[slot].page;
        reserved_[slot] = reserved_[--reserved_count_];
        break;
      }
    }
    assert(page != nullptr && "an operation allocates only pages it reserved");
    node_latch latch(page);
    allocated_[allocated_count_++] = {page, latch.lock_exclusive()};
    return page;
  }

  void deallocate(page_kind kind, void* page, page_layout /*layout*/) const noexcept {
    freed_[freed_count_++] = {static_cast<std::byte*>(page), kind};
  }

 private:
  static constexpr std::size_t max_pages = 2 * (23 + 2);
  static constexpr page_layout layout_of(page_kind kind) noexcept {
    return kind == page_kind::leaf ? T::leaf.page : T::internal.page;
  }

  struct reserved {
    std::byte* page;
    page_kind kind;
  };
  struct allocated {
    std::byte* page;
    latch_version version;
  };

  free_lists<T>& free_;
  const page_source<A>& source_;
  mutable reserved reserved_[max_pages] = {};
  mutable std::size_t reserved_count_ = 0;
  mutable allocated allocated_[max_pages] = {};
  mutable std::size_t allocated_count_ = 0;
  mutable reserved freed_[max_pages] = {};
  mutable std::size_t freed_count_ = 0;
};

}  // namespace bbtree::detail
