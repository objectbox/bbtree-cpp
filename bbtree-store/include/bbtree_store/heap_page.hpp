// `heap_page`: a page of records from the allocator's heap member
// (`page_allocator_traits::allocate_heap`), of the node config's heap page
// size, aligned to its size, so that a record finds its page by rounding its
// address down.
//
//   | next | top | live | records ... | room |
//
// - Records go one after another, from the header to `top`, each where `top`
//   was. One that dies leaves a hole, until its leaf slides the page's
//   records together (`leaf_heap.hpp`).
// - A chain of pages is a leaf's (`leaf_heap.hpp`): `next` links each to the
//   one before it. Only the thread holding the chain's leaf exclusively reads
//   or writes `next` and `top`, and frees the page.
// - `live` counts the bytes of the page's live records. A record of another
//   leaf may lie in a leaf's chain, once entries move between leaves; a
//   thread holding that other leaf kills it, through `live`, an atomic.
// - An orphan is a page no chain holds any more: a compaction moved its
//   leaf's records out, and other leaves' are left. `live` keeps a bit for it,
//   so that whichever thread empties an orphan, by its last kill or by
//   orphaning it, frees it: one of them, once.
#pragma once

#include <bbtree/allocator.hpp>
#include <bbtree/detail/node/keys.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

namespace bbtree_store::detail {

// A heap page of node config `C`, by its start; null for none.
template <class C>
class heap_page {
 public:
  // A page's size and alignment: a power of two, from 1 KiB to 1 GiB.
  static constexpr std::size_t size = bbtree::detail::heap_page_size_of<C>;
  static_assert((size & (size - 1)) == 0 && size >= 1024 && size <= (std::size_t{1} << 30),
                "a node config's heap_page_size is a power of two, from 1 KiB to 1 GiB");

 private:
  // A page's header, at its start.
  struct page_header {
    // The page before this one in its chain; null at the chain's end.
    std::byte* next;
    // Where the next record goes, from the page's start.
    std::uint32_t top;
    // The bytes of the page's live records, and `orphan_bit` once no chain
    // holds it.
    std::atomic<std::uint32_t> live;
  };

  // Where the records start.
  static constexpr std::size_t records_offset = sizeof(page_header);

  // The bit of `live` that marks an orphan.
  static constexpr std::uint32_t orphan_bit = std::uint32_t{1} << 31;

 public:
  // The largest record a page holds.
  static constexpr std::size_t room_for_records = size - records_offset;

  static constexpr bbtree::page_layout layout{size, size};

  heap_page() noexcept = default;

  // The page at `page`; none for null.
  explicit heap_page(std::byte* page) noexcept : page_(page) {}

  // A new page from `allocator`, with no record, in no chain yet; none if the
  // allocator refuses.
  template <class A>
  static heap_page allocate(const A& allocator) noexcept {
    auto* page = static_cast<std::byte*>(bbtree::page_allocator_traits<A>::allocate_heap(allocator, layout));
    if (page != nullptr) {
      ::new (static_cast<void*>(page)) page_header{nullptr, static_cast<std::uint32_t>(records_offset), {0}};
    }
    return heap_page(page);
  }

  // Gives the page back to `allocator`, which it came from: its records are
  // dead, and it is in no chain, so no thread reads it again.
  template <class A>
  void deallocate(const A& allocator) const noexcept {
    header()->~page_header();
    bbtree::page_allocator_traits<A>::deallocate_heap(allocator, page_, layout);
  }

  // The page that holds the record at `record`.
  static heap_page of_record(const std::byte* record) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(record) & ~(std::uintptr_t{size} - 1);
    return heap_page(reinterpret_cast<std::byte*>(address));
  }

  explicit operator bool() const noexcept { return page_ != nullptr; }
  std::byte* address() const noexcept { return page_; }
  friend bool operator==(heap_page, heap_page) = default;

  // The fields below are the chain's holder's alone (top of the file): the
  // caller holds the page's leaf exclusively.

  // The page before this one in its chain.
  heap_page next() const noexcept { return heap_page(header()->next); }

  // Links `next` after this page in its chain.
  void set_next(heap_page next) const noexcept { header()->next = next.page_; }

  // The bytes free after the last record.
  std::size_t room() const noexcept { return size - top(); }

  // The bytes records took: the live ones' and the holes'.
  std::size_t used() const noexcept { return top() - records_offset; }

  // The bytes of the page's live records.
  std::size_t live() const noexcept { return header()->live.load(std::memory_order_acquire) & ~orphan_bit; }

  // Room for a record of `record_size` bytes, live, after the last one; the
  // page has the room.
  std::byte* append(std::size_t record_size) const noexcept {
    assert(record_size <= room() && "a record goes where it fits");
    const std::size_t at = top();
    header()->top = static_cast<std::uint32_t>(at + record_size);
    header()->live.fetch_add(static_cast<std::uint32_t>(record_size), std::memory_order_relaxed);
    return page_ + at;
  }

  // Where the first record goes.
  std::byte* records_start() const noexcept { return page_ + records_offset; }

  // Whether the record at `record` lies in this page.
  bool holds(const std::byte* record) const noexcept { return of_record(record) == *this; }

  // Sets where the next record goes to `used_bytes` past the first: the
  // page's records, once slid together, end there.
  void set_used(std::size_t used_bytes) const noexcept {
    assert(used_bytes <= room_for_records);
    header()->top = static_cast<std::uint32_t>(records_offset + used_bytes);
  }

  // `moved` bytes of live records moved from this page to `to`, both of one
  // chain, where only its leaf's records live.
  void move_live(heap_page to, std::size_t moved) const noexcept {
    if (*this != to) {
      header()->live.fetch_sub(static_cast<std::uint32_t>(moved), std::memory_order_relaxed);
      to.header()->live.fetch_add(static_cast<std::uint32_t>(moved), std::memory_order_relaxed);
    }
  }

  // `killed` bytes of the page's live records died. Whether the page is an
  // orphan they left empty, which the caller frees.
  bool kill(std::size_t killed) const noexcept {
    const auto bytes = static_cast<std::uint32_t>(killed);
    const std::uint32_t before = header()->live.fetch_sub(bytes, std::memory_order_acq_rel);
    return before - bytes == orphan_bit;
  }

  // Lets the page go from its chain, which no longer holds it. Whether it was
  // empty, which the caller frees.
  bool orphan() const noexcept {
    set_next(heap_page());
    return header()->live.fetch_or(orphan_bit, std::memory_order_acq_rel) == 0;
  }

 private:
  page_header* header() const noexcept { return std::launder(reinterpret_cast<page_header*>(page_)); }
  std::size_t top() const noexcept { return header()->top; }

  std::byte* page_ = nullptr;
};

}  // namespace bbtree_store::detail
