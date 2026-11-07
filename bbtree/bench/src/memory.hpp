// Memory: the counting allocator that memory mode wraps around mimalloc, the
// mode each binary runs in, and the process's resident set: the Rust bench's
// `memory.rs`.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace bbtree_bench {

// mimalloc, counting the bytes it has live. The memory binary routes every
// `operator new` and `delete` through its one instance (`memory_main.cpp`).
//
// It counts the bytes mimalloc hands out for each block (`mi_usable_size`),
// not the bytes asked for, as the Rust bench's `CountingAllocator` does: an
// unsized `operator delete`, which bbtree's `global_allocator` and much of
// the std library call, gives no size to subtract (PORTING.md, P17).
class counting_allocator {
 public:
  constexpr counting_allocator() noexcept = default;
  counting_allocator(const counting_allocator&) = delete;
  counting_allocator& operator=(const counting_allocator&) = delete;

  // Null when mimalloc has no memory, for the `nothrow` forms.
  void* allocate(std::size_t size) noexcept;
  void* allocate_aligned(std::size_t size, std::size_t alignment) noexcept;

  // `std::bad_alloc` when mimalloc has no memory.
  void* allocate_or_throw(std::size_t size);
  void* allocate_aligned_or_throw(std::size_t size, std::size_t alignment);

  void deallocate(void* pointer) noexcept;

  // The bytes that destroying `value` frees, which are the bytes it held on
  // the heap, including what it owns indirectly.
  template <class T>
  std::size_t bytes_freed_by_dropping(T&& value) noexcept {
    const std::size_t before = live_bytes_.load(std::memory_order_relaxed);
    { [[maybe_unused]] const auto dropped = std::forward<T>(value); }
    return before - live_bytes_.load(std::memory_order_relaxed);
  }

 private:
  void count_allocated(void* pointer) noexcept;

  std::atomic<std::size_t> live_bytes_{0};
};

// What each cell measures, which the binary decides by its allocator.
class run_mode {
 public:
  // Time, on plain mimalloc.
  static run_mode timing() noexcept { return run_mode(nullptr, "mimalloc"); }

  // Time, on the system's allocator (glibc's `malloc` on Linux): what a map
  // gets by default where nothing replaces `operator new`.
  static run_mode timing_on_system_allocator() noexcept {
    return run_mode(nullptr, "the system allocator (no replacement of operator new)");
  }

  // The memory a cell's maps hold, from the counting allocator.
  static run_mode memory(counting_allocator& allocator) noexcept {
    return run_mode(&allocator, "mimalloc, wrapped in a live-byte counter");
  }

  bool is_memory() const noexcept { return allocator_ != nullptr; }

  counting_allocator& allocator() const noexcept { return *allocator_; }

  std::string_view name() const noexcept { return is_memory() ? "memory" : "timing"; }

  // The global allocator, for report headers.
  std::string_view allocator_description() const noexcept { return allocator_description_; }

 private:
  run_mode(counting_allocator* allocator, std::string_view allocator_description) noexcept
      : allocator_(allocator), allocator_description_(allocator_description) {}

  counting_allocator* allocator_;
  std::string_view allocator_description_;
};

// The process's resident set, from `/proc/self/status`; zero where that
// isn't available.
class resident_set {
 public:
  static resident_set read();

  std::uint64_t current_bytes() const noexcept { return current_bytes_; }
  std::uint64_t peak_bytes() const noexcept { return peak_bytes_; }

 private:
  resident_set(std::uint64_t current, std::uint64_t peak) noexcept
      : current_bytes_(current), peak_bytes_(peak) {}

  // Reads a `Name:   1234 kB` line.
  static std::uint64_t field_bytes(std::string_view status, std::string_view name) noexcept;

  std::uint64_t current_bytes_;
  std::uint64_t peak_bytes_;
};

}  // namespace bbtree_bench
