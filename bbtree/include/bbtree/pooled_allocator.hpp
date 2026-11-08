// A page allocator that packs a map's pages into chunks (PORTING.md, P32):
// pages of one layout are carved one after another from chunks that double
// from one page to 2 MiB, and pages given back are kept for reuse.
//
// Why: the global allocator asks aligned `operator new` for every page, and
// glibc's `malloc` places 1 KiB pages aligned to 1 KiB with gaps between
// them and without huge pages, which makes walks, builds and lookups miss
// the TLB far more than they do on mimalloc. The idea of packing pages comes
// from stx_btree, another C++ form of bbtree; the 2 MiB chunks on huge
// pages are this port's.
//
// Ownership: copies of a `pooled_allocator` share one pool, which lives
// until the last copy goes; only then do its chunks go back to the system.
// A map that shrinks keeps its pages for its next growth, as a
// `std::vector` keeps its capacity.
//
// Threads: the pool takes a mutex for each page, so maps on different
// threads may share one, and so may the shared map's threads; with 8 of
// them inserting it was faster than the global allocator on glibc, and
// slower on mimalloc (P32).
#pragma once

#include <bbtree/allocator.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace bbtree {

namespace detail {

// The pages of one pool: a size class per page layout, each carving its
// pages from chunks of its own, and every chunk, which the pool frees when it
// ends.
class page_pool {
 public:
  page_pool() = default;
  page_pool(const page_pool&) = delete;
  page_pool& operator=(const page_pool&) = delete;

  ~page_pool() {
    for (const chunk& owned : chunks_) {
      owned.release();
    }
  }

  // A page of `layout`, zeroed if `zeroed`; null when the system has no
  // memory. Layouts larger than a chunk go to `operator new` directly.
  void* allocate(page_layout layout, bool zeroed) noexcept {
    if (layout.size == 0) {
      return nullptr;
    }
    if (!pools(layout)) {
      void* page = ::operator new(layout.size, std::align_val_t{layout.align}, std::nothrow);
      if (page != nullptr && zeroed) {
        std::memset(page, 0, layout.size);
      }
      return page;
    }
    const std::lock_guard lock(mutex_);
    size_class* pages = class_of(layout);
    if (pages == nullptr) {
      return nullptr;
    }
    if (pages->free_list != nullptr) {
      void* page = pages->free_list;
      std::memcpy(&pages->free_list, page, sizeof(void*));
      if (zeroed) {
        std::memset(page, 0, layout.size);
      }
      return page;
    }
    if (pages->next == pages->end && !add_chunk(*pages)) {
      return nullptr;
    }
    std::byte* page = pages->next;
    pages->next += layout.size;
    if (zeroed && !pages->chunk_is_zeroed) {
      std::memset(page, 0, layout.size);
    }
    return page;
  }

  void deallocate(void* page, page_layout layout) noexcept {
    if (!pools(layout)) {
      ::operator delete(page, std::align_val_t{layout.align});
      return;
    }
    const std::lock_guard lock(mutex_);
    size_class* pages = class_of(layout);
    // A page this pool handed out has its class already.
    std::memcpy(page, &pages->free_list, sizeof(void*));
    pages->free_list = page;
  }

 private:
  // The largest chunk: a huge page on x86-64.
  static constexpr std::size_t largest_chunk_bytes = std::size_t{2} << 20;

  // Whether pages of `layout` come from chunks: those that pack back to back
  // at their alignment and hold a free list's link; others, and pages larger
  // than a quarter of a chunk, come from `operator new` directly.
  static constexpr bool pools(page_layout layout) noexcept {
    return layout.size >= sizeof(void*) && layout.size % layout.align == 0 &&
           layout.size <= largest_chunk_bytes / 4;
  }

  struct size_class {
    page_layout layout;
    void* free_list = nullptr;
    std::byte* next = nullptr;
    std::byte* end = nullptr;
    // The next chunk's size, in pages: doubles from one, so that a map of a
    // few entries takes no more than the global allocator would.
    std::size_t next_chunk_pages = 1;
    // Whether the chunk being carved came zeroed from the system.
    bool chunk_is_zeroed = false;
  };

  struct chunk {
    void* base;
    std::size_t bytes;
    std::size_t align;
    bool mapped;

    void release() const noexcept {
#if defined(__linux__)
      if (mapped) {
        ::munmap(base, bytes);
        return;
      }
#endif
      ::operator delete(base, std::align_val_t{align});
    }
  };

  size_class* class_of(page_layout layout) noexcept {
    for (size_class& pages : classes_) {
      if (pages.layout == layout) {
        return &pages;
      }
    }
    try {
      return &classes_.emplace_back(size_class{.layout = layout});
    } catch (const std::bad_alloc&) {
      return nullptr;
    }
  }

  bool add_chunk(size_class& pages) noexcept {
    const std::size_t page_bytes = pages.layout.size;
    const std::size_t bytes =
        std::min(pages.next_chunk_pages * page_bytes, largest_chunk_bytes / page_bytes * page_bytes);
    try {
      chunks_.reserve(chunks_.size() + 1);
    } catch (const std::bad_alloc&) {
      return false;
    }
    chunk owned{};
    // The last doubling reaches a whole huge page.
    if (bytes > largest_chunk_bytes / 2 && map_huge_chunk(owned)) {
      pages.chunk_is_zeroed = true;
    } else {
      void* base = ::operator new(bytes, std::align_val_t{pages.layout.align}, std::nothrow);
      if (base == nullptr) {
        return false;
      }
      owned = chunk{base, bytes, pages.layout.align, false};
      pages.chunk_is_zeroed = false;
    }
    chunks_.push_back(owned);
    pages.next = static_cast<std::byte*>(owned.base);
    pages.end = pages.next + std::min(bytes, owned.bytes) / page_bytes * page_bytes;
    pages.next_chunk_pages = std::min(pages.next_chunk_pages * 2, largest_chunk_bytes / page_bytes);
    return true;
  }

  // A 2 MiB chunk aligned to 2 MiB, which Linux backs with a huge page where
  // transparent huge pages are on for regions that ask (their default mode,
  // `madvise`); zeroed, as fresh mappings are.
  static bool map_huge_chunk(chunk& owned) noexcept {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    const std::size_t bytes = largest_chunk_bytes;
    void* mapped = ::mmap(nullptr, 2 * bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
      return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(mapped);
    const std::uintptr_t aligned = (start + bytes - 1) & ~(bytes - 1);
    if (aligned > start) {
      ::munmap(mapped, aligned - start);
    }
    const std::uintptr_t tail = aligned + bytes;
    if (start + 2 * bytes > tail) {
      ::munmap(reinterpret_cast<void*>(tail), start + 2 * bytes - tail);
    }
    void* base = reinterpret_cast<void*>(aligned);
    ::madvise(base, bytes, MADV_HUGEPAGE);
    // Written once now, so that the chunk faults in as one huge page: the
    // shared map's first touch of a page is a read, which would map the
    // kernel's huge zero page, and the write after it would then split it
    // into 4 KiB pages. One fault here, under the pool's mutex, instead of
    // 512 under the map's latches; the byte written is a zero, so the chunk
    // stays zeroed.
    *static_cast<volatile std::byte*>(base) = std::byte{0};
    owned = chunk{base, bytes, bytes, true};
    return true;
#else
    (void)owned;
    return false;
#endif
  }

  std::mutex mutex_;
  std::vector<size_class> classes_;
  std::vector<chunk> chunks_;
};

}  // namespace detail

// A page allocator whose copies share one pool of packed pages (above).
//
// Constructing one allocates its pool, which throws `std::bad_alloc` if
// there is no memory; a map default-constructed on it, whose constructor
// is `noexcept`, then ends the program. To handle that, construct the
// allocator first and pass it to the map. The pool is made at once, not at
// the first page, so that copies made before then share it.
class pooled_allocator {
 public:
  pooled_allocator() : pool_(std::make_shared<detail::page_pool>()) {}

  void* allocate_zeroed(page_layout layout) const noexcept { return pool_->allocate(layout, true); }
  void deallocate(void* page, page_layout layout) const noexcept { pool_->deallocate(page, layout); }

  // Heap pages (bbtree-store's) need not be zeroed.
  void* allocate_heap(page_layout layout) const noexcept { return pool_->allocate(layout, false); }
  void deallocate_heap(void* page, page_layout layout) const noexcept { pool_->deallocate(page, layout); }

  friend bool operator==(const pooled_allocator&, const pooled_allocator&) = default;

 private:
  std::shared_ptr<detail::page_pool> pool_;
};

}  // namespace bbtree
