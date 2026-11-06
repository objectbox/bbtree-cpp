// A cell's untimed setup on all cores: copying its keys into input pools,
// making values, and sorting: the Rust bench's `parallel_setup.rs`, on
// `std::thread` instead of rayon. Timed regions stay on the cell's own
// thread, and cells still run one at a time.
//
// Work on fewer than `min_items` items stays on the calling thread, so that
// small maps' keys and values stay in its heap: small maps' timed regions
// free them (removals, pops, upserts of present keys), and a free from
// another thread's heap takes mimalloc's slower cross-thread path. Larger
// inputs' allocations land in the worker threads' heaps, as in the Rust
// bench.
//
// Shuffles stay serial (`seeded_rng::shuffle`): the Rust bench spreads them
// only for speed, with the serial result.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace bbtree_bench {

class parallel_setup {
 public:
  // The fewest items worth spreading over all cores: below it, the work
  // takes well under a millisecond on one.
  static constexpr std::size_t min_items = std::size_t{1} << 16;

  // Whether work on `item_count` items runs on all cores.
  static constexpr bool spreads(std::size_t item_count) noexcept { return item_count >= min_items; }

  // `make(index, item)` for each of `items`, in order.
  template <class T, class Make>
  static auto map(std::span<const T> items, const Make& make) {
    using result = std::invoke_result_t<const Make&, std::size_t, const T&>;
    static_assert(std::default_initializable<result>, "results are made in place");
    std::vector<result> results(items.size());
    over_chunks(items.size(), [&](std::size_t begin, std::size_t end) {
      for (std::size_t index = begin; index < end; ++index) {
        results[index] = make(index, items[index]);
      }
    });
    return results;
  }

  // A copy of each of `items`, in order.
  template <class T>
  static std::vector<T> cloned(std::span<const T> items) {
    return map(items, [](std::size_t, const T& item) { return item; });
  }

  // Sorts `items` as `std::sort` does: which of equal items comes first may
  // differ, so callers sort items that are distinct. Spread: each thread
  // sorts a chunk, then neighbouring chunks merge, in rounds.
  template <class T, class Less>
  static void sort(std::vector<T>& items, const Less& less) {
    if (!spreads(items.size())) {
      std::sort(items.begin(), items.end(), less);
      return;
    }
    const std::size_t chunk_count = thread_count();
    std::vector<std::size_t> bounds;
    for (std::size_t chunk = 0; chunk <= chunk_count; ++chunk) {
      bounds.push_back(items.size() * chunk / chunk_count);
    }
    on_threads(chunk_count, [&](std::size_t chunk) {
      std::sort(items.begin() + offset(bounds[chunk]), items.begin() + offset(bounds[chunk + 1]), less);
    });
    for (std::size_t width = 1; width < chunk_count; width *= 2) {
      const std::size_t merge_count = (chunk_count + 2 * width - 1) / (2 * width);
      on_threads(merge_count, [&](std::size_t merge) {
        const std::size_t first = merge * 2 * width;
        const std::size_t middle = std::min(first + width, chunk_count);
        const std::size_t last = std::min(first + 2 * width, chunk_count);
        std::inplace_merge(items.begin() + offset(bounds[first]), items.begin() + offset(bounds[middle]),
                           items.begin() + offset(bounds[last]), less);
      });
    }
  }

 private:
  static std::size_t thread_count() noexcept { return std::max(1u, std::thread::hardware_concurrency()); }

  static std::ptrdiff_t offset(std::size_t index) noexcept { return static_cast<std::ptrdiff_t>(index); }

  // Runs `work(begin, end)` over `item_count` items: in contiguous chunks on
  // every core, or on this thread for few items.
  template <class Work>
  static void over_chunks(std::size_t item_count, const Work& work) {
    if (!spreads(item_count)) {
      work(std::size_t{0}, item_count);
      return;
    }
    const std::size_t chunk_count = thread_count();
    on_threads(chunk_count, [&](std::size_t chunk) {
      work(item_count * chunk / chunk_count, item_count * (chunk + 1) / chunk_count);
    });
  }

  // Runs `work(task)` for each of `task_count` tasks, each on a thread of
  // its own, and waits for all of them.
  template <class Work>
  static void on_threads(std::size_t task_count, const Work& work) {
    std::vector<std::jthread> threads;
    threads.reserve(task_count);
    for (std::size_t task = 0; task < task_count; ++task) {
      threads.emplace_back([&work, task] { work(task); });
    }
  }
};

}  // namespace bbtree_bench
