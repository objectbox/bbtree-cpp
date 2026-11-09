// `pooled_allocator` (PORTING.md, P32): its pages, its pool shared by copies,
// and maps on it, of one thread and shared by threads.
#include <bbtree/map.hpp>
#include <bbtree/pooled_allocator.hpp>
#include <bbtree/unsync/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool all_zero(const void* page, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(page);
  for (std::size_t b = 0; b < size; ++b) {
    if (bytes[b] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("a pooled allocator hands out zeroed, aligned, distinct pages", "[allocator][pooled]") {
  const bbtree::pooled_allocator allocator;
  const bbtree::page_layout layout{1024, 1024};
  std::set<void*> pages;
  // Past the first chunks and into a 2 MiB one.
  for (int i = 0; i < 5000; ++i) {
    void* page = allocator.allocate_zeroed(layout);
    REQUIRE(page != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(page) % layout.align == 0);
    REQUIRE(all_zero(page, layout.size));
    std::memset(page, 0xAB, layout.size);
    CHECK(pages.insert(page).second);
  }
  for (void* page : pages) {
    allocator.deallocate(page, layout);
  }
}

TEST_CASE("a pooled allocator reuses a page given back, zeroed again", "[allocator][pooled]") {
  const bbtree::pooled_allocator allocator;
  const bbtree::page_layout layout{512, 512};
  void* page = allocator.allocate_zeroed(layout);
  std::memset(page, 0xCD, layout.size);
  allocator.deallocate(page, layout);
  void* again = allocator.allocate_zeroed(layout);
  CHECK(again == page);
  CHECK(all_zero(again, layout.size));
  allocator.deallocate(again, layout);
}

TEST_CASE("copies of a pooled allocator share its pool", "[allocator][pooled]") {
  const bbtree::pooled_allocator original;
  const bbtree::pooled_allocator copy = original;
  CHECK(copy == original);
  CHECK_FALSE(bbtree::pooled_allocator() == original);
  const bbtree::page_layout layout{1024, 1024};
  void* page = original.allocate_zeroed(layout);
  copy.deallocate(page, layout);
  CHECK(copy.allocate_zeroed(layout) == page);
  original.deallocate(page, layout);
}

TEST_CASE("a pooled allocator moved from keeps its pool", "[allocator][pooled]") {
  using pooled_map = bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t, std::less<std::uint64_t>,
                                                bbtree::pooled_allocator>;
  bbtree::pooled_allocator original;
  const bbtree::pooled_allocator moved_to = std::move(original);
  CHECK(moved_to == original);  // NOLINT(bugprone-use-after-move): the state under test
  bbtree::pooled_allocator assigned;
  assigned = std::move(original);
  CHECK(assigned == original);  // NOLINT(bugprone-use-after-move)
  const bbtree::page_layout layout{1024, 64};
  void* page = original.allocate_zeroed(layout);
  REQUIRE(page != nullptr);
  moved_to.deallocate(page, layout);
  pooled_map map(original);
  for (std::uint64_t key = 0; key < 1000; ++key) {
    map.insert({key, key});
  }
  CHECK(map.size() == 1000);
}

TEST_CASE("a pooled allocator passes odd and large layouts to operator new", "[allocator][pooled]") {
  const bbtree::pooled_allocator allocator;
  CHECK(allocator.allocate_zeroed({0, 8}) == nullptr);
  for (const bbtree::page_layout layout : {bbtree::page_layout{4, 4}, bbtree::page_layout{24, 16},
                                           bbtree::page_layout{std::size_t{1} << 20, 4096}}) {
    void* page = allocator.allocate_zeroed(layout);
    REQUIRE(page != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(page) % layout.align == 0);
    CHECK(all_zero(page, layout.size));
    allocator.deallocate(page, layout);
  }
  // Heap pages come unzeroed, of any layout.
  void* heap = allocator.allocate_heap({4000, 8});
  REQUIRE(heap != nullptr);
  allocator.deallocate_heap(heap, {4000, 8});
}

TEST_CASE("maps on a pooled allocator, alone and sharing one, match std::map", "[allocator][pooled]") {
  using pooled_map = bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t, std::less<std::uint64_t>,
                                                bbtree::pooled_allocator>;
  const bbtree::pooled_allocator shared;
  std::vector<pooled_map> maps;
  maps.emplace_back();  // a pool of its own
  for (int i = 0; i < 8; ++i) {
    maps.emplace_back(shared);
  }
  std::vector<std::map<std::uint64_t, std::uint64_t>> expected(maps.size());
  std::mt19937_64 rng(42);
  for (int op = 0; op < 200000; ++op) {
    const std::size_t m = rng() % maps.size();
    const std::uint64_t key = rng() % 20000;
    if (rng() % 3 == 0) {
      CHECK(maps[m].erase(key) == expected[m].erase(key));
    } else {
      maps[m].insert_or_assign(key, key * 3);
      expected[m].insert_or_assign(key, key * 3);
    }
  }
  for (std::size_t m = 0; m < maps.size(); ++m) {
    REQUIRE(maps[m].size() == expected[m].size());
    CHECK(
        std::equal(maps[m].begin(), maps[m].end(), expected[m].begin(), expected[m].end(),
                   [](const auto& a, const auto& b) { return a.first == b.first && a.second == b.second; }));
  }
  // A clone shares the pool and outlives the map it came from.
  pooled_map clone = maps[1];
  maps.clear();
  CHECK(clone.size() == expected[1].size());
}

TEST_CASE("the shared map's threads share one pooled allocator", "[allocator][pooled][threads]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t, std::less<std::uint64_t>, bbtree::pooled_allocator> map;
  constexpr int threads = 4;
  constexpr std::uint64_t per_thread = 50000;
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&map, t] {
      for (std::uint64_t i = 0; i < per_thread; ++i) {
        map.insert(i * threads + static_cast<std::uint64_t>(t), i);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  for (std::uint64_t key = 0; key < per_thread * threads; key += 997) {
    CHECK(map.get_copied(key) == key / threads);
  }
}
