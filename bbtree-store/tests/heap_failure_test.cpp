// A store while the program's heap fails. A compaction takes its lists from
// the global heap, the store's only memory outside its allocator's pages:
// `std::bad_alloc` there leaves an insert or a removal with the store as it
// was and every page owned, and doesn't fail an insert whose value went in.
//
// An executable of its own: it replaces the global `operator new`, to fail
// one allocation of the test's choosing.
#include <bbtree_store/store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace {

// Which of the calling thread's allocations fails next, if any: the one
// switch the replaced `operator new` reads, which nothing can hand it.
class failing_heap {
 public:
  // Fails the allocation `after` allocations from now, once.
  static void fail_after(std::size_t after) noexcept { countdown_ = after; }
  // Fails none; whether the one `fail_after` set did fail.
  static bool disarm() noexcept {
    const bool failed = !countdown_.has_value();
    countdown_.reset();
    return failed;
  }
  // Whether this allocation is the one to fail.
  static bool fails_now() noexcept {
    if (!countdown_) {
      return false;
    }
    if (*countdown_ > 0) {
      --*countdown_;
      return false;
    }
    countdown_.reset();
    return true;
  }

 private:
  static constinit thread_local std::optional<std::size_t> countdown_;
};

constinit thread_local std::optional<std::size_t> failing_heap::countdown_;

}  // namespace

// The allocating forms the standard library's defaults don't route here use
// aligned storage, as the store's pages do: those never fail.
void* operator new(std::size_t size) {
  if (failing_heap::fails_now()) {
    throw std::bad_alloc();
  }
  if (void* block = std::malloc(size != 0 ? size : 1)) {
    return block;
  }
  throw std::bad_alloc();
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t /*size*/) noexcept { std::free(block); }

namespace {

using bbtree_store::bytes_of;
using bbtree_store::insert_result;
using byte_string = std::vector<std::uint8_t>;

// Heap pages out; pages come from the global allocator.
struct heap_page_count {
  long out = 0;
};

class counting_pages {
 public:
  explicit counting_pages(heap_page_count& count) noexcept : count_(&count) {}

  void* allocate_zeroed(bbtree::page_layout layout) const noexcept {
    return bbtree::global_allocator{}.allocate_zeroed(layout);
  }
  void deallocate(void* page, bbtree::page_layout layout) const noexcept {
    bbtree::global_allocator{}.deallocate(page, layout);
  }
  void* allocate_heap(bbtree::page_layout layout) const noexcept {
    ++count_->out;
    return bbtree::global_allocator{}.allocate_zeroed(layout);
  }
  void deallocate_heap(void* page, bbtree::page_layout layout) const noexcept {
    --count_->out;
    bbtree::global_allocator{}.deallocate(page, layout);
  }

 private:
  heap_page_count* count_;
};

using counted_store = bbtree_store::bbtree_store<counting_pages>;

// The keys "k0" to "k8". With a value of 100 bytes a record takes 111: eight
// fill a 1 KiB heap page but for 120 bytes.
std::string name(int n) { return "k" + std::to_string(n); }

// A store whose one leaf's chain is a page of "k0" to "k7" with 100-byte
// values, `removed` of them removed again, from "k0" on.
void fill_page(counted_store& store, int removed) {
  const byte_string hundred(100, 1);
  for (int n = 0; n < 8; ++n) {
    REQUIRE(store.insert(bytes_of(name(n)), hundred) == insert_result::inserted);
  }
  for (int n = 0; n < removed; ++n) {
    REQUIRE(store.remove(bytes_of(name(n))));
  }
}

}  // namespace

TEST_CASE("a heap failure in an insert's compaction changes nothing and loses no page", "[store][heap]") {
  // Three of eight records gone leave a chain a quarter holes, which an
  // insert that finds no room compacts. The value takes three overflow
  // pages, written before the leaf is held.
  const byte_string long_value(4'000, 2);
  std::size_t failures = 0;
  for (std::size_t after = 0;; ++after) {
    heap_page_count pages;
    bool threw = false;
    {
      counted_store store{counting_pages(pages)};
      fill_page(store, 3);
      const long pages_before = pages.out;
      failing_heap::fail_after(after);
      try {
        const insert_result result = store.insert(bytes_of("k8"), long_value);
        (void)failing_heap::disarm();
        REQUIRE(result == insert_result::inserted);
      } catch (const std::bad_alloc&) {
        (void)failing_heap::disarm();
        threw = true;
      }
      if (threw) {
        // The overflow pages went back, and the store holds what it held.
        CHECK(pages.out == pages_before);
        CHECK_FALSE(store.contains(bytes_of("k8")));
        for (int n = 3; n < 8; ++n) {
          CHECK(store.get(bytes_of(name(n))) == byte_string(100, 1));
        }
        CHECK(store.insert(bytes_of("k8"), long_value) == insert_result::inserted);
      }
      CHECK(store.get(bytes_of("k8")) == long_value);
    }
    CHECK(pages.out == 0);
    if (!threw) {
      break;
    }
    ++failures;
  }
  // The compaction's lists are several allocations: each failed once.
  CHECK(failures >= 4);
}

TEST_CASE("a heap failure after a value was replaced doesn't fail the insert", "[store][heap]") {
  // Records of 111 and 113 bytes, the first removed: just over half the page
  // is live, so nothing compacts yet. A value one byte longer for the second
  // finds room after them, and its old record's death leaves the page half
  // holes, which the insert compacts once the value is replaced.
  const byte_string before(102, 1);
  const byte_string longer(103, 3);
  std::size_t failures = 0;
  for (std::size_t after = 0;; ++after) {
    heap_page_count pages;
    bool failed = false;
    {
      counted_store store{counting_pages(pages)};
      REQUIRE(store.insert(bytes_of("k0"), byte_string(100, 1)) == insert_result::inserted);
      REQUIRE(store.insert(bytes_of("k1"), before) == insert_result::inserted);
      REQUIRE(store.remove(bytes_of("k0")));
      failing_heap::fail_after(after);
      const insert_result result = store.insert(bytes_of("k1"), longer);
      failed = failing_heap::disarm();
      CHECK(result == insert_result::replaced);
      CHECK(store.get(bytes_of("k1")) == longer);
      CHECK(pages.out == 1);
    }
    CHECK(pages.out == 0);
    if (!failed) {
      break;
    }
    ++failures;
  }
  // The compaction ran into each of its lists' allocations once.
  CHECK(failures >= 2);
}

TEST_CASE("a heap failure in a removal's compaction leaves the key", "[store][heap]") {
  // A removal that leaves its page half holes, as the fifth of eight records
  // does, compacts first.
  std::size_t failures = 0;
  for (std::size_t after = 0;; ++after) {
    heap_page_count pages;
    bool threw = false;
    {
      counted_store store{counting_pages(pages)};
      fill_page(store, 4);
      failing_heap::fail_after(after);
      try {
        const bool removed = store.remove(bytes_of("k4"));
        (void)failing_heap::disarm();
        REQUIRE(removed);
      } catch (const std::bad_alloc&) {
        (void)failing_heap::disarm();
        threw = true;
      }
      if (threw) {
        CHECK(store.get(bytes_of("k4")) == byte_string(100, 1));
        CHECK(store.remove(bytes_of("k4")));
      }
      CHECK_FALSE(store.contains(bytes_of("k4")));
      for (int n = 5; n < 8; ++n) {
        CHECK(store.get(bytes_of(name(n))) == byte_string(100, 1));
      }
    }
    CHECK(pages.out == 0);
    if (!threw) {
      break;
    }
    ++failures;
  }
  CHECK(failures >= 4);
}
