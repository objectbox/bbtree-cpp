// A store's insert, get and scan times at one thread, inserts and gets at
// eight, and the bytes its pages take, by kind; then the heap's bytes under
// churn, as values change size and entries go and come back; then values too
// long for a record, in overflow pages. One store at a time. A port of the
// Rust crate's `examples/bench.rs`, on mimalloc as it is.
//
//   cmake --preset bench && cmake --build --preset bench
//   build/bench/bbtree-store/bbtree-store-bench [entries]
#include <mimalloc-new-delete.h>
#include <bbtree_store/store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

// Bytes out, nodes' and heap pages', from the global allocator.
struct page_counts {
  std::atomic<std::size_t> nodes{0};
  std::atomic<std::size_t> heap{0};
};

class counting_allocator {
 public:
  explicit counting_allocator(page_counts& counts) noexcept : counts_(&counts) {}

  void* allocate_zeroed(bbtree::page_layout layout) const noexcept {
    counts_->nodes.fetch_add(layout.size, std::memory_order_relaxed);
    return bbtree::global_allocator().allocate_zeroed(layout);
  }
  void deallocate(void* page, bbtree::page_layout layout) const noexcept {
    counts_->nodes.fetch_sub(layout.size, std::memory_order_relaxed);
    bbtree::global_allocator().deallocate(page, layout);
  }
  void* allocate_heap(bbtree::page_layout layout) const noexcept {
    counts_->heap.fetch_add(layout.size, std::memory_order_relaxed);
    return bbtree::global_allocator().allocate_zeroed(layout);
  }
  void deallocate_heap(void* page, bbtree::page_layout layout) const noexcept {
    counts_->heap.fetch_sub(layout.size, std::memory_order_relaxed);
    bbtree::global_allocator().deallocate(page, layout);
  }

 private:
  page_counts* counts_;
};

using store = bbtree_store::bbtree_store<counting_allocator>;
using bytes = std::span<const std::uint8_t>;

class store_bench {
 public:
  explicit store_bench(std::uint64_t entries) noexcept : entries_(entries) {}

  void run() const {
    std::printf("%llu entries of 20-byte keys and 32-byte values\n",
                static_cast<unsigned long long>(entries_));
    {
      page_counts counts;
      store shared{counting_allocator(counts)};
      one_thread(shared, counts);
      churn(shared, counts);
    }
    threads();
    long_values();
  }

 private:
  using clock = std::chrono::steady_clock;

  // Entry `i`'s key: 20 bytes, in an order apart from `i`'s.
  static std::array<std::uint8_t, 20> key(std::uint64_t i) noexcept {
    std::array<std::uint8_t, 20> key;
    key.fill('k');
    const std::uint64_t scrambled = i * 0x9E37'79B9'7F4A'7C15ull;
    for (std::size_t byte = 0; byte < 8; ++byte) {
      key[byte] = static_cast<std::uint8_t>(scrambled >> (56 - 8 * byte));
    }
    return key;
  }

  // Entry `i`'s value: 32 bytes.
  static std::array<std::uint8_t, 32> value(std::uint64_t i) noexcept {
    std::array<std::uint8_t, 32> value;
    value.fill(static_cast<std::uint8_t>(i));
    return value;
  }

  static double ns_per(clock::time_point start, std::uint64_t count) noexcept {
    return std::chrono::duration<double, std::nano>(clock::now() - start).count() /
           static_cast<double>(count);
  }

  double per_entry(const std::atomic<std::size_t>& count) const noexcept {
    return static_cast<double>(count.load(std::memory_order_relaxed)) / static_cast<double>(entries_);
  }

  static void insert(store& into, bytes key, bytes value) {
    const auto result = into.insert(key, value);
    if (result != bbtree_store::insert_result::inserted && result != bbtree_store::insert_result::replaced) {
      throw std::runtime_error("no budget");
    }
  }

  static void expect(bool holds, const char* what) {
    if (!holds) {
      throw std::runtime_error(what);
    }
  }

  void one_thread(store& shared, const page_counts& counts) const {
    auto start = clock::now();
    for (std::uint64_t i = 0; i < entries_; ++i) {
      insert(shared, key(i), value(i));
    }
    const double insert_ns = ns_per(start, entries_);

    std::vector<std::uint8_t> buf;
    start = clock::now();
    for (std::uint64_t i = 0; i < entries_; ++i) {
      buf.clear();
      expect(shared.get_into(key(i * 7919 % entries_), buf).has_value(), "every key is in the store");
    }
    const double get_ns = ns_per(start, entries_);

    start = clock::now();
    std::size_t scanned = 0;
    shared.scan(bbtree_store::key_range::all(), [&](bytes k, bytes v) {
      scanned += k.size() + v.size();
      return true;
    });
    const double scan_ns = ns_per(start, entries_);
    expect(scanned == entries_ * (20 + 32), "the scan visits every byte");

    const double nodes = per_entry(counts.nodes);
    const double heap = per_entry(counts.heap);
    std::printf(
        "1 thread:  insert %.0f ns, get %.0f ns, scan %.1f ns an entry; nodes %.1f + heap %.1f = %.1f bytes "
        "an "
        "entry\n",
        insert_ns, get_ns, scan_ns, nodes, heap, nodes + heap);
  }

  // Churn over the store's keys: each op replaces a value with one of 16 to
  // 64 bytes, or removes the entry, or puts a removed one back.
  void churn(store& shared, const page_counts& counts) const {
    std::vector<bool> present(entries_, true);
    std::size_t live_bytes = entries_ * (20 + 32 + 9);
    std::uint64_t state = 0x2545'F491'4F6C'DD1Dull;
    std::vector<std::uint8_t> old;
    const auto start = clock::now();
    for (std::uint64_t round = 1; round <= 4; ++round) {
      for (std::uint64_t op = 0; op < entries_; ++op) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const std::uint64_t i = state % entries_;
        const std::size_t len = 16 + (state >> 40) % 49;
        old.clear();
        if (present[i] && state >> 60 < 4) {
          shared.get_into(key(i), old);
          expect(shared.remove(key(i)), "a present key is removed");
          live_bytes -= 20 + 9 + old.size();
          present[i] = false;
        } else {
          const bool had = shared.get_into(key(i), old).has_value();
          const std::vector<std::uint8_t> new_value(len, static_cast<std::uint8_t>(i));
          insert(shared, key(i), new_value);
          live_bytes += 20 + 9 + len;
          if (had) {
            live_bytes -= 20 + 9 + old.size();
          }
          present[i] = true;
        }
      }
      const double heap = static_cast<double>(counts.heap.load(std::memory_order_relaxed));
      std::printf("churn round %llu: heap %.2f bytes a live record byte, %.0f ns an op\n",
                  static_cast<unsigned long long>(round), heap / static_cast<double>(live_bytes),
                  ns_per(start, entries_ * round));
    }
  }

  void threads() const {
    constexpr std::uint64_t thread_count = 8;
    page_counts counts;
    store shared{counting_allocator(counts)};
    auto start = clock::now();
    {
      std::vector<std::jthread> workers;
      for (std::uint64_t t = 0; t < thread_count; ++t) {
        workers.emplace_back([&, t] {
          for (std::uint64_t i = t; i < entries_; i += thread_count) {
            insert(shared, key(i), value(i));
          }
        });
      }
    }
    const double insert_ns = ns_per(start, entries_);
    std::atomic<std::uint64_t> misses{0};
    start = clock::now();
    {
      std::vector<std::jthread> workers;
      for (std::uint64_t t = 0; t < thread_count; ++t) {
        workers.emplace_back([&, t] {
          std::vector<std::uint8_t> buf;
          for (std::uint64_t i = t; i < entries_; i += thread_count) {
            buf.clear();
            if (!shared.get_into(key(i * 7919 % entries_), buf)) {
              misses.fetch_add(1, std::memory_order_relaxed);
            }
          }
        });
      }
    }
    const double get_ns = ns_per(start, entries_);
    expect(misses.load() == 0, "every key is in the store");
    std::printf("8 threads: insert %.0f ns, get %.0f ns an entry, wall time\n", insert_ns, get_ns);
  }

  // A sixteenth as many entries, of 4,000-byte values: three overflow pages
  // each, and 952 bytes in the record.
  void long_values() const {
    constexpr std::size_t len = 4'000;
    const std::uint64_t entries = entries_ / 16;
    page_counts counts;
    store shared{counting_allocator(counts)};
    std::vector<std::uint8_t> long_value(len);
    auto start = clock::now();
    for (std::uint64_t i = 0; i < entries; ++i) {
      std::fill(long_value.begin(), long_value.end(), static_cast<std::uint8_t>(i));
      insert(shared, key(i), long_value);
    }
    const double insert_ns = ns_per(start, entries);
    std::vector<std::uint8_t> buf;
    start = clock::now();
    for (std::uint64_t i = 0; i < entries; ++i) {
      buf.clear();
      expect(shared.get_into(key(i * 7919 % entries), buf) == len, "every long value comes back whole");
    }
    const double get_ns = ns_per(start, entries);
    const double heap =
        static_cast<double>(counts.heap.load(std::memory_order_relaxed)) / static_cast<double>(entries * len);
    std::printf(
        "%llu entries of %zu-byte values: insert %.0f ns, get %.0f ns; heap %.3f bytes a value byte\n",
        static_cast<unsigned long long>(entries), len, insert_ns, get_ns, heap);
  }

  std::uint64_t entries_;
};

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t entries = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
  store_bench(entries).run();
}
