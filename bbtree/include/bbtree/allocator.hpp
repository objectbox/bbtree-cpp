// Where a map's memory comes from (D8): an allocator, the map's last template
// parameter, as in std's containers. The default is `global_allocator`.
//
// A map asks it for pages of two kinds, internal nodes' and leaves', each by
// a member of its own; the root page, which holds the root node of either
// kind, is a page of the larger kind. A third kind, heap pages, is for the
// code that keeps a map's keys and values beside it (bbtree-store). Every
// page of one kind in a map has one layout. By default every kind comes from
// `allocate_zeroed`; an allocator that tells the kinds apart, to budget, pool
// or place them apart, defines the kinds' members.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <new>

namespace bbtree {

// The size and alignment of a page; both non-zero, the alignment a power of
// two.
struct page_layout {
  std::size_t size;
  std::size_t align;

  friend constexpr bool operator==(page_layout, page_layout) = default;
};

// What a page is for, which tells which of an allocator's members hands it
// out and takes it back.
enum class page_kind : unsigned char { internal, leaf };

// An allocator of the pages a map keeps its nodes in.
//
// Contract (the Rust trait's safety section):
// - A page an allocating member returns is valid for reads and writes of
//   `layout.size` bytes, aligned to `layout.align`, overlaps no other live
//   page, and is all zero (but for heap pages, which may hold anything).
//   Failure is a null pointer.
// - It stays valid until given back to its kind's deallocating member, with
//   the layout it was asked for with. A copy of the allocator is the same
//   allocator: it takes back what the original handed out.
// - No member throws: a map calls them in the middle of changing its nodes.
template <class A>
concept page_allocator = std::copy_constructible<A> && requires(const A& a, void* p, page_layout l) {
  { a.allocate_zeroed(l) } noexcept -> std::same_as<void*>;
  { a.deallocate(p, l) } noexcept;
};

// The global allocator: aligned `operator new`, zeroed. What a map uses
// unless given another.
struct global_allocator {
  void* allocate_zeroed(page_layout layout) const noexcept {
    if (layout.size == 0) {
      return nullptr;
    }
    void* page = ::operator new(layout.size, std::align_val_t{layout.align}, std::nothrow);
    if (page != nullptr) {
      std::memset(page, 0, layout.size);
    }
    return page;
  }

  void deallocate(void* page, page_layout layout) const noexcept {
    ::operator delete(page, std::align_val_t{layout.align});
  }

  friend constexpr bool operator==(global_allocator, global_allocator) = default;
};

// The members a map calls, each defaulting to `allocate_zeroed`/`deallocate`
// when the allocator doesn't define its own.
template <page_allocator A>
struct page_allocator_traits {
  static void* allocate(const A& a, page_kind kind, page_layout layout) noexcept {
    if (kind == page_kind::internal) {
      if constexpr (requires { a.allocate_internal(layout); }) {
        return a.allocate_internal(layout);
      } else {
        return a.allocate_zeroed(layout);
      }
    }
    if constexpr (requires { a.allocate_leaf(layout); }) {
      return a.allocate_leaf(layout);
    } else {
      return a.allocate_zeroed(layout);
    }
  }

  static void deallocate(const A& a, page_kind kind, void* page, page_layout layout) noexcept {
    if (kind == page_kind::internal) {
      if constexpr (requires { a.deallocate_internal(page, layout); }) {
        a.deallocate_internal(page, layout);
      } else {
        a.deallocate(page, layout);
      }
      return;
    }
    if constexpr (requires { a.deallocate_leaf(page, layout); }) {
      a.deallocate_leaf(page, layout);
    } else {
      a.deallocate(page, layout);
    }
  }

  static void* allocate_heap(const A& a, page_layout layout) noexcept {
    if constexpr (requires { a.allocate_heap(layout); }) {
      return a.allocate_heap(layout);
    } else {
      return a.allocate_zeroed(layout);
    }
  }

  static void deallocate_heap(const A& a, void* page, page_layout layout) noexcept {
    if constexpr (requires { a.deallocate_heap(page, layout); }) {
      a.deallocate_heap(page, layout);
    } else {
      a.deallocate(page, layout);
    }
  }
};

// Refers to an allocator that outlives it, to share one between maps (the
// Rust crate's `&A`).
template <page_allocator A>
class allocator_ref {
 public:
  explicit allocator_ref(const A& allocator) noexcept : allocator_(&allocator) {}

  void* allocate_zeroed(page_layout layout) const noexcept { return allocator_->allocate_zeroed(layout); }
  void deallocate(void* page, page_layout layout) const noexcept { allocator_->deallocate(page, layout); }
  void* allocate_internal(page_layout layout) const noexcept {
    return traits::allocate(*allocator_, page_kind::internal, layout);
  }
  void* allocate_leaf(page_layout layout) const noexcept {
    return traits::allocate(*allocator_, page_kind::leaf, layout);
  }
  void deallocate_internal(void* page, page_layout layout) const noexcept {
    traits::deallocate(*allocator_, page_kind::internal, page, layout);
  }
  void deallocate_leaf(void* page, page_layout layout) const noexcept {
    traits::deallocate(*allocator_, page_kind::leaf, page, layout);
  }
  void* allocate_heap(page_layout layout) const noexcept {
    return traits::allocate_heap(*allocator_, layout);
  }
  void deallocate_heap(void* page, page_layout layout) const noexcept {
    traits::deallocate_heap(*allocator_, page, layout);
  }

 private:
  using traits = page_allocator_traits<A>;

  const A* allocator_;
};

// An allocator had no page to give (the Rust crate's `AllocError`).
class alloc_error : public std::bad_alloc {
 public:
  const char* what() const noexcept override { return "bbtree: memory allocation failed"; }
};

}  // namespace bbtree
