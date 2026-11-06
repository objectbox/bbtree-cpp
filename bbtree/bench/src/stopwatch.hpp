// The stopwatch that times a cell's regions, and the optimizer barrier that
// keeps the work in them: the Rust bench's `Stopwatch` and `black_box`
// (`operations.rs`).
#pragma once

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>

#include "perf_counters.hpp"

namespace bbtree_bench {

// What `std::hint::black_box` does for the Rust bench: the compiler must
// assume a value is read, or a pointer's target changed, here.
class optimizer_barrier {
 public:
  template <class T>
  static void keep(const T& value) noexcept {
    if constexpr (std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(void*)) {
      asm volatile("" : : "r,m"(value) : "memory");
    } else {
      asm volatile("" : : "m"(value) : "memory");
    }
  }

  // `pointer`, as something the compiler can't see through, so that passes
  // over its target aren't merged.
  template <class T>
  static T* opaque(T* pointer) noexcept {
    asm volatile("" : "+r"(pointer) : : "memory");
    return pointer;
  }
};

// Adds up the time of a cell's timed regions. Under `--perf`, it also
// switches perf's counters on and off around each region, so the counters
// cover the same work as the time.
class stopwatch {
 public:
  explicit stopwatch(std::optional<perf_control> control) noexcept : perf_control_(std::move(control)) {}

  // Runs `region` timed. Its result goes through the optimizer barrier, so
  // the work can't be optimized away; destroying the result is not timed.
  template <class Region>
  auto time(Region&& region) {
    if (perf_control_) {
      perf_control_->start_counting();
    }
    const auto start = std::chrono::steady_clock::now();
    auto result = std::forward<Region>(region)();
    optimizer_barrier::keep(result);
    const auto end = std::chrono::steady_clock::now();
    if (perf_control_) {
      perf_control_->stop_counting();
    }
    elapsed_ += end - start;
    return result;
  }

  std::chrono::nanoseconds elapsed() const noexcept { return elapsed_; }

 private:
  std::chrono::nanoseconds elapsed_{0};
  std::optional<perf_control> perf_control_;
};

}  // namespace bbtree_bench
