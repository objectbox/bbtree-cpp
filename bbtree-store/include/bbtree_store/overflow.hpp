// `overflow`: the pages of a value too long for its record (`record.hpp`),
// heap pages of their own (`page_allocator_traits::allocate_heap`), each the
// address of the next, then value bytes, every one full but, perhaps, the
// last.
//
//   | next | value bytes ... |
//
// The pages are their record's: a record that moves (`leaf_heap.hpp`) takes
// its link to them along, and leaves them where they are; they go back when
// it dies. No chain holds them, and only their record points to them, so a
// thread reads them only while it holds the record's leaf, as it reads the
// record.
#pragma once

#include <bbtree_store/heap_page.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace bbtree_store::detail {

// A value's overflow pages, by the first, of node config `C`; null for none.
template <class C>
class overflow {
  // The size of a page's link to the next.
  static constexpr std::size_t link_size = sizeof(std::byte*);

 public:
  // The value bytes a page holds.
  static constexpr std::size_t room = heap_page<C>::size - link_size;

  overflow() noexcept = default;

  // The pages a record's link points to.
  explicit overflow(std::byte* first) noexcept : first_(first) {}

  // Writes `bytes`, not empty, into new pages from `allocator`; none, with the
  // pages it took given back, if one is refused.
  template <class A>
  static overflow write(std::span<const std::uint8_t> bytes, const A& allocator) noexcept {
    assert(!bytes.empty() && "an overflow holds some bytes");
    // From the last page back to the first, each linking the next.
    overflow next;
    const std::size_t pages = (bytes.size() + room - 1) / room;
    for (std::size_t part = pages; part-- > 0;) {
      auto* page = static_cast<std::byte*>(
          bbtree::page_allocator_traits<A>::allocate_heap(allocator, heap_page<C>::layout));
      if (page == nullptr) {
        next.free(allocator);
        return overflow();
      }
      const std::size_t start = part * room;
      const std::size_t len = std::min(room, bytes.size() - start);
      std::memcpy(page, &next.first_, link_size);
      std::memcpy(page + link_size, bytes.data() + start, len);
      next = overflow(page);
    }
    return next;
  }

  explicit operator bool() const noexcept { return first_ != nullptr; }

  // The link to the pages, which their record keeps.
  std::byte* link() const noexcept { return first_; }

  // Appends the `len` bytes the pages hold to `buf`. Their record is read, so
  // they stay.
  void append_to(std::size_t len, std::vector<std::uint8_t>& buf) const {
    const std::byte* page = first_;
    std::size_t left = len;
    while (left > 0) {
      const std::size_t part = std::min(left, room);
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(page + link_size);
      buf.insert(buf.end(), bytes, bytes + part);
      left -= part;
      if (left > 0) {
        std::memcpy(&page, page, link_size);
      }
    }
  }

  // Gives the pages back to `allocator`, which they came from: their record
  // is dead, or never went in, so no thread reads them again.
  template <class A>
  void free(const A& allocator) const noexcept {
    std::byte* page = first_;
    while (page != nullptr) {
      std::byte* next = nullptr;
      std::memcpy(&next, page, link_size);
      bbtree::page_allocator_traits<A>::deallocate_heap(allocator, page, heap_page<C>::layout);
      page = next;
    }
  }

 private:
  std::byte* first_ = nullptr;
};

}  // namespace bbtree_store::detail
