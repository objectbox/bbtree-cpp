// A page allocator for tests (the Rust crate's `TestPages`): it knows every
// page it handed out, so a page that comes back twice, from another
// allocator, with another layout or through another kind's member ends the
// test (`lifetime_violation`), and a test can ask how many are out. It can
// also refuse: after a set number of pages every request fails, which is how
// the tests reach each allocation point of an operation in turn.
#pragma once

#include "instrumented.hpp"

#include <bbtree/allocator.hpp>

#include <cstddef>
#include <cstring>
#include <new>
#include <set>
#include <unordered_map>

namespace bbtree_test {

// The pages a `checked_allocator` has out, and how many it may still give.
class page_book {
 public:
  // Which of the allocator's members a page came through.
  enum class member : std::uint8_t { internal, leaf, any_kind };

  page_book() = default;
  page_book(const page_book&) = delete;
  page_book& operator=(const page_book&) = delete;

  void* hand_out(member through, bbtree::page_layout layout) noexcept {
    if (pages_left_ == 0) {
      ++refused_;
      return nullptr;
    }
    if (pages_left_ > 0) {
      --pages_left_;
    }
    void* page = ::operator new(layout.size, std::align_val_t{layout.align}, std::nothrow);
    if (page == nullptr) {
      lifetime_violation::report("the test ran out of memory");
    }
    std::memset(page, 0, layout.size);
    out_.emplace(page, record{layout, through});
    sizes_asked_for_.insert(layout.size);
    ++handed_out_;
    return page;
  }

  void take_back(member through, void* page, bbtree::page_layout layout) noexcept {
    const auto found = out_.find(page);
    if (found == out_.end()) {
      lifetime_violation::report("a page was given back twice, or to an allocator it didn't come from");
    }
    if (!(found->second.layout == layout)) {
      lifetime_violation::report("a page was given back with another layout");
    }
    if (found->second.through != through) {
      lifetime_violation::report("a page was given back through another kind's member");
    }
    out_.erase(found);
    // Whatever still reads the page reads nonsense, and under ASan fails.
    std::memset(page, 0xDD, layout.size);
    ::operator delete(page, std::align_val_t{layout.align});
  }

  std::size_t in_use() const noexcept { return out_.size(); }
  // How many pages it ever handed out.
  std::size_t handed_out() const noexcept { return handed_out_; }
  std::size_t refused() const noexcept { return refused_; }
  const std::set<std::size_t>& sizes_asked_for() const noexcept { return sizes_asked_for_; }

  // Hands out `count` more pages; the requests after fail.
  void refuse_after(long count) noexcept { pages_left_ = count; }
  void refuse_nothing() noexcept { pages_left_ = unlimited; }

 private:
  static constexpr long unlimited = -1;
  struct record {
    bbtree::page_layout layout;
    member through;
  };

  std::unordered_map<void*, record> out_;
  std::set<std::size_t> sizes_asked_for_;
  std::size_t handed_out_ = 0;
  std::size_t refused_ = 0;
  long pages_left_ = unlimited;
};

// The allocator of the maps under test: every page through its book.
class checked_allocator {
 public:
  explicit checked_allocator(page_book& book) noexcept : book_(&book) {}

  void* allocate_zeroed(bbtree::page_layout layout) const noexcept {
    return book_->hand_out(page_book::member::any_kind, layout);
  }
  void deallocate(void* page, bbtree::page_layout layout) const noexcept {
    book_->take_back(page_book::member::any_kind, page, layout);
  }
  void* allocate_internal(bbtree::page_layout layout) const noexcept {
    return book_->hand_out(page_book::member::internal, layout);
  }
  void deallocate_internal(void* page, bbtree::page_layout layout) const noexcept {
    book_->take_back(page_book::member::internal, page, layout);
  }
  void* allocate_leaf(bbtree::page_layout layout) const noexcept {
    return book_->hand_out(page_book::member::leaf, layout);
  }
  void deallocate_leaf(void* page, bbtree::page_layout layout) const noexcept {
    book_->take_back(page_book::member::leaf, page, layout);
  }

  page_book& book() const noexcept { return *book_; }
  friend bool operator==(const checked_allocator&, const checked_allocator&) = default;

 private:
  page_book* book_;
};
static_assert(bbtree::page_allocator<checked_allocator>);

}  // namespace bbtree_test
