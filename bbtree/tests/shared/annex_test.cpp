// Leaf annexes (D60): an annex that holds a heap page for each entry its leaf
// took, as a store's chain holds its records' pages, follows its leaf through
// splits, merges and teardown, every page given back once; closures that
// refuse leave the map as it was; and annexes are neither lost nor doubled
// while threads change the tree. (The Rust crate's `node/annexed_leaf.rs`
// tests.)
#include <bbtree/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Pages out, by kind; pages come from the global allocator.
struct page_counts {
  std::atomic<long> nodes{0};
  std::atomic<long> heap{0};
  // What the annexes counted, summed as the map released them.
  std::atomic<long> released{0};
};

// Counts the pages it hands out, so a test can check that every one went
// back. Copies count into the same tally, as copies of one allocator.
class counting_pages {
 public:
  explicit counting_pages(page_counts& counts) noexcept : counts_(&counts) {}

  void* allocate_zeroed(bbtree::page_layout layout) const noexcept {
    ++counts_->nodes;
    return bbtree::global_allocator{}.allocate_zeroed(layout);
  }
  void deallocate(void* page, bbtree::page_layout layout) const noexcept {
    --counts_->nodes;
    bbtree::global_allocator{}.deallocate(page, layout);
  }
  void* allocate_heap(bbtree::page_layout layout) const noexcept {
    ++counts_->heap;
    return bbtree::global_allocator{}.allocate_zeroed(layout);
  }
  void deallocate_heap(void* page, bbtree::page_layout layout) const noexcept {
    --counts_->heap;
    bbtree::global_allocator{}.deallocate(page, layout);
  }
  void record_release(long counted) const noexcept { counts_->released += counted; }

 private:
  page_counts* counts_;
};

// The layout of the test annex's heap pages.
constexpr bbtree::page_layout heap_page{64, 64};

// An annex that holds a heap page for each entry its leaf took: a split hands
// the new leaf half of them, a merge takes the other leaf's, and the map's
// teardown gives them back.
class page_per_entry {
 public:
  void split(page_per_entry& right) noexcept {
    const auto half = pages_.begin() + static_cast<std::ptrdiff_t>(pages_.size() / 2);
    right.pages_.assign(half, pages_.end());
    pages_.erase(half, pages_.end());
  }
  void merge(page_per_entry&& right) noexcept {
    pages_.insert(pages_.end(), right.pages_.begin(), right.pages_.end());
    right.pages_.clear();
  }
  template <class A>
  void release(const A& allocator) && noexcept {
    for (void* page : pages_) {
      bbtree::page_allocator_traits<A>::deallocate_heap(allocator, page, heap_page);
    }
    pages_.clear();
  }

  void take(void* page) { pages_.push_back(page); }
  void* give_back() noexcept {
    if (pages_.empty()) {
      return nullptr;
    }
    void* page = pages_.back();
    pages_.pop_back();
    return page;
  }

 private:
  std::vector<void*> pages_;
};

}  // namespace

// A vector of pages holds no pointer into itself: it moves with its leaf's
// bytes (P6).
template <>
struct bbtree::is_trivially_relocatable<page_per_entry> : std::true_type {};

namespace {

static_assert(bbtree::leaf_annex_for<page_per_entry, counting_pages>);

// An annex that counts its leaf's inserts less its removals: a split hands
// the new leaf half, a merge adds the other leaf's, and the teardown records
// what each held. Their sum is conserved: once the map goes, the released
// counts add up to the entries it held, if no annex was lost or doubled.
class entry_count {
 public:
  void split(entry_count& right) noexcept {
    right.count_ = count_ / 2;
    count_ -= right.count_;
  }
  void merge(entry_count&& right) noexcept { count_ += std::exchange(right.count_, 0); }
  void release(const counting_pages& allocator) && noexcept { allocator.record_release(count_); }

  void add(long change) noexcept { count_ += change; }

 private:
  long count_ = 0;
};

using annexed_map = bbtree::bbtree_map<std::uint64_t, std::uint64_t, std::less<std::uint64_t>, counting_pages,
                                       bbtree::with_leaf_annex<bbtree::auto_inline_keys, page_per_entry>>;
using held_leaf = annexed_map::annexed_leaf_type;

// Whether the leaf's keys are in order, and hold `key` exactly when `holds`.
bool keys_fit(const held_leaf& leaf, std::uint64_t key, bool holds) {
  bool found = false;
  for (std::size_t index = 0; index < leaf.key_count(); ++index) {
    if (index > 0 && !(leaf.key(index - 1) < leaf.key(index))) {
      return false;
    }
    found = found || leaf.key(index) == key;
  }
  return found == holds;
}

// Inserts `key`, taking a heap page into the annex of the leaf it goes in;
// whether it was new.
bool insert(annexed_map& map, const counting_pages& pages, std::uint64_t key, std::uint64_t value) {
  return map.update_or_insert_with_leaf(
      key, value,
      [&](held_leaf leaf, const std::uint64_t& stored, std::uint64_t& stored_value) {
        REQUIRE(keys_fit(leaf, stored, true));
        stored_value = value;
        return false;
      },
      [&](held_leaf leaf, const std::uint64_t& new_key, const std::uint64_t&) {
        REQUIRE(keys_fit(leaf, new_key, false));
        void* page = bbtree::page_allocator_traits<counting_pages>::allocate_heap(pages, heap_page);
        if (page == nullptr) {
          throw bbtree::alloc_error();
        }
        leaf.annex().take(page);
        return true;
      });
}

// Removes `key`, giving a heap page of its leaf's annex back, if it holds
// one: the map's teardown gives the rest back.
std::optional<std::uint64_t> remove(annexed_map& map, const counting_pages& pages, std::uint64_t key) {
  auto removed =
      map.remove_with_leaf(key, [&](held_leaf leaf, const std::uint64_t& stored, const std::uint64_t&) {
        REQUIRE(keys_fit(leaf, stored, true));
        if (void* page = leaf.annex().give_back()) {
          bbtree::page_allocator_traits<counting_pages>::deallocate_heap(pages, page, heap_page);
        }
      });
  if (!removed) {
    return std::nullopt;
  }
  REQUIRE(removed->first == key);
  return removed->second;
}

// Random inserts and removals against `model`, with every leaf's annex taking
// and giving pages.
void churn(annexed_map& map, const counting_pages& pages, std::map<std::uint64_t, std::uint64_t>& model,
           std::uint64_t seed, int steps, std::uint64_t keys) {
  std::mt19937_64 rng(seed);
  for (int step = 0; step < steps; ++step) {
    const std::uint64_t key = rng() % keys;
    const auto value = static_cast<std::uint64_t>(step);
    if (rng() % 3 < 2) {
      const bool is_new = insert(map, pages, key, value);
      REQUIRE(is_new == !model.contains(key));
      model[key] = value;
    } else {
      const std::optional<std::uint64_t> removed = remove(map, pages, key);
      const auto expected = model.find(key);
      REQUIRE(removed.has_value() == (expected != model.end()));
      if (removed) {
        REQUIRE(*removed == expected->second);
        model.erase(expected);
      }
    }
  }
}

void require_holds(const annexed_map& map, const std::map<std::uint64_t, std::uint64_t>& model) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> scanned;
  map.scan([&](const std::uint64_t& key, const std::uint64_t& value) {
    scanned.emplace_back(key, value);
    return true;
  });
  const std::vector<std::pair<std::uint64_t, std::uint64_t>> expected(model.begin(), model.end());
  REQUIRE(scanned == expected);
}

}  // namespace

TEST_CASE("annexes follow their leaves through splits, merges and teardown", "[shared][annex]") {
  page_counts counts;
  const counting_pages pages(counts);
  {
    annexed_map map(pages);
    std::map<std::uint64_t, std::uint64_t> model;
    churn(map, pages, model, 7, 30'000, 3'000);
    map.check_invariants(model.size());
    require_holds(map, model);
    CHECK(counts.heap.load() > 0);
    // Clearing releases every annex; the map takes entries again after.
    map.clear();
    CHECK(counts.heap.load() == 0);
    model.clear();
    churn(map, pages, model, 8, 10'000, 1'000);
    map.check_invariants(model.size());
    require_holds(map, model);
  }
  CHECK(counts.heap.load() == 0);
  CHECK(counts.nodes.load() == 0);
}

TEST_CASE("a refused insert changes nothing", "[shared][annex]") {
  page_counts counts;
  const counting_pages pages(counts);
  {
    annexed_map map(pages);
    std::map<std::uint64_t, std::uint64_t> model;
    std::mt19937_64 rng(9);
    const std::uint64_t count = 2'000;
    for (std::uint64_t step = 0; step < count; ++step) {
      const std::uint64_t key = rng() % (2 * count);
      insert(map, pages, key, step);
      model[key] = step;
    }
    const long heap_before = counts.heap.load();
    // New keys land in leaves with room, full leaves that would split, and
    // after the last key; held keys refuse on the update.
    for (std::uint64_t key = 0; key < 2 * count + 100; key += 3) {
      bool refused = false;
      try {
        (void)map.update_or_insert_with_leaf(
            key, 0,
            [](held_leaf, const std::uint64_t&, std::uint64_t&) -> bool { throw bbtree::alloc_error(); },
            [](held_leaf, const std::uint64_t&, const std::uint64_t&) -> bool {
              throw bbtree::alloc_error();
            });
      } catch (const bbtree::alloc_error&) {
        refused = true;
      }
      REQUIRE(refused);
    }
    CHECK(counts.heap.load() == heap_before);
    map.check_invariants(model.size());
    for (const auto& [key, value] : model) {
      REQUIRE(map.get_copied(key) == value);
    }
    require_holds(map, model);
  }
  CHECK(counts.heap.load() == 0);
  CHECK(counts.nodes.load() == 0);
}

TEST_CASE("annexes keep their counts while threads insert and remove", "[shared][annex]") {
  using counted_map =
      bbtree::bbtree_map<std::uint64_t, std::uint64_t, std::less<std::uint64_t>, counting_pages,
                         bbtree::with_leaf_annex<bbtree::auto_inline_keys, entry_count>>;
  using counted_leaf = counted_map::annexed_leaf_type;
  page_counts counts;
  const counting_pages pages(counts);
  constexpr int threads = 4;
  constexpr std::uint64_t per_thread = 20'000;
  std::uint64_t held = 0;
  {
    counted_map map(pages);
    std::vector<std::thread> workers;
    // Catch2's assertions are not thread-safe: workers count failures.
    std::atomic<int> wrong{0};
    for (int t = 0; t < threads; ++t) {
      workers.emplace_back([&, t] {
        std::mt19937_64 rng(static_cast<std::uint64_t>(t) + 11);
        for (std::uint64_t i = 0; i < per_thread; ++i) {
          // Thread t owns the keys k * threads + t.
          const std::uint64_t key = (rng() % 4'000) * threads + static_cast<std::uint64_t>(t);
          if (rng() % 3 < 2) {
            (void)map.update_or_insert_with_leaf(
                key, key, [](counted_leaf, const std::uint64_t&, std::uint64_t&) { return false; },
                [](counted_leaf leaf, const std::uint64_t&, const std::uint64_t&) {
                  leaf.annex().add(1);
                  return true;
                });
          } else {
            const auto removed = map.remove_with_leaf(
                key,
                [](counted_leaf leaf, const std::uint64_t&, const std::uint64_t&) { leaf.annex().add(-1); });
            if (removed && removed->second != key) {
              wrong.fetch_add(1);
            }
          }
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    CHECK(wrong.load() == 0);
    map.scan([&](const std::uint64_t&, const std::uint64_t&) {
      ++held;
      return true;
    });
    map.check_invariants(held);
  }
  CHECK(counts.released.load() == static_cast<long>(held));
  CHECK(counts.nodes.load() == 0);
}
