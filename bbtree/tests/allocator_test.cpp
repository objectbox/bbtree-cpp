#include <bbtree/allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace {

// Counts the pages of each kind it hands out, and takes them from the global
// allocator.
struct counting_allocator {
  struct counts {
    int internal = 0;
    int leaf = 0;
    int other = 0;
  };
  counts* tally;

  void* allocate_zeroed(bbtree::page_layout layout) const noexcept {
    ++tally->other;
    return bbtree::global_allocator{}.allocate_zeroed(layout);
  }
  void deallocate(void* page, bbtree::page_layout layout) const noexcept {
    bbtree::global_allocator{}.deallocate(page, layout);
  }
  void* allocate_leaf(bbtree::page_layout layout) const noexcept {
    ++tally->leaf;
    return bbtree::global_allocator{}.allocate_zeroed(layout);
  }
};

}  // namespace

TEST_CASE("the global allocator hands out zeroed, aligned pages", "[allocator]") {
  const bbtree::global_allocator allocator;
  const bbtree::page_layout layout{1024, 1024};
  std::vector<void*> pages;
  for (int i = 0; i < 16; ++i) {
    void* page = allocator.allocate_zeroed(layout);
    REQUIRE(page != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(page) % 1024 == 0);
    const auto* bytes = static_cast<const unsigned char*>(page);
    for (std::size_t b = 0; b < layout.size; ++b) {
      REQUIRE(bytes[b] == 0);
    }
    pages.push_back(page);
  }
  for (void* page : pages) {
    allocator.deallocate(page, layout);
  }
}

TEST_CASE("the global allocator refuses zero-sized pages", "[allocator]") {
  CHECK(bbtree::global_allocator{}.allocate_zeroed({0, 8}) == nullptr);
}

TEST_CASE("a kind's member defaults to allocate_zeroed unless defined", "[allocator]") {
  counting_allocator::counts tally;
  const counting_allocator allocator{&tally};
  using traits = bbtree::page_allocator_traits<counting_allocator>;
  const bbtree::page_layout layout{512, 512};
  void* leaf = traits::allocate(allocator, bbtree::page_kind::leaf, layout);
  void* internal = traits::allocate(allocator, bbtree::page_kind::internal, layout);
  void* heap = traits::allocate_heap(allocator, layout);
  CHECK(tally.leaf == 1);
  CHECK(tally.other == 2);
  traits::deallocate(allocator, bbtree::page_kind::leaf, leaf, layout);
  traits::deallocate(allocator, bbtree::page_kind::internal, internal, layout);
  traits::deallocate_heap(allocator, heap, layout);
}

TEST_CASE("an allocator_ref forwards to the allocator it refers to", "[allocator]") {
  counting_allocator::counts tally;
  const counting_allocator allocator{&tally};
  const bbtree::allocator_ref shared(allocator);
  static_assert(bbtree::page_allocator<decltype(shared)>);
  void* page = shared.allocate_leaf({1024, 1024});
  CHECK(tally.leaf == 1);
  shared.deallocate_leaf(page, {1024, 1024});
}
