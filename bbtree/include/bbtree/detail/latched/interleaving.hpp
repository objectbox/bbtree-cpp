// Where a latched tree's threads may interleave (the Rust crate's
// `tree/interleaving.rs`): before each access to an atomic that threads
// share, which are a latch word, the root and last-leaf pointers and a free
// list (`interleaved`); before each racing read (`racing_reads`, latch.hpp);
// and in each round of waiting for another thread (`backoff`, latch.hpp).
//
// In every build but a test's these points are nothing: `interleaved<A>` is
// `A`, and `interleaving`'s members are empty. A test built with
// `BBTREE_INTERLEAVING` defines them (`tests/support/interleaving_schedule`):
// its scheduler runs the test's threads one at a time and switches between
// them at these points, by a seed, so that it reaches schedules real threads
// reach once in billions of runs, and reaches them again from the seed. One
// thread runs at a time, so every access is sequentially consistent: it
// finds bad interleavings, not reorderings a weak memory model allows.
#pragma once

#if defined(BBTREE_INTERLEAVING)
#include <atomic>
#include <type_traits>
#endif

namespace bbtree::detail {

#if defined(BBTREE_INTERLEAVING)

struct interleaving {
  // Another thread may run before the caller goes on.
  static void switch_point() noexcept;
  // One round of waiting for another thread to let go of what the caller
  // needs: lets another thread run, and says whether a scheduler runs the
  // caller at all. A thread that none runs waits by spinning.
  static bool let_another_run() noexcept;
};

// An atomic that threads share: every access is a switch point first.
template <class Atomic>
class interleaved {
 public:
  using value_type = typename Atomic::value_type;

  // As `Atomic` is made: a `std::atomic` of its first value, a
  // `std::atomic_ref` of the object it refers to.
  explicit interleaved(value_type initial) noexcept
    requires std::is_constructible_v<Atomic, value_type>
      : atomic_(initial) {}
  explicit interleaved(value_type& referred) noexcept
    requires(!std::is_constructible_v<Atomic, value_type>)
      : atomic_(referred) {}

  value_type load(std::memory_order order) const noexcept {
    interleaving::switch_point();
    return atomic_.load(order);
  }
  void store(value_type value, std::memory_order order) noexcept {
    interleaving::switch_point();
    atomic_.store(value, order);
  }
  bool compare_exchange_strong(value_type& expected, value_type desired, std::memory_order success,
                               std::memory_order failure) noexcept {
    interleaving::switch_point();
    return atomic_.compare_exchange_strong(expected, desired, success, failure);
  }
  // Strong: a spurious failure would make a schedule depend on more than
  // its seed.
  bool compare_exchange_weak(value_type& expected, value_type desired, std::memory_order success,
                             std::memory_order failure) noexcept {
    return compare_exchange_strong(expected, desired, success, failure);
  }
  value_type fetch_sub(value_type value, std::memory_order order) noexcept {
    interleaving::switch_point();
    return atomic_.fetch_sub(value, order);
  }

 private:
  Atomic atomic_;
};

#else

struct interleaving {
  [[gnu::always_inline]] static void switch_point() noexcept {}
  [[gnu::always_inline]] static bool let_another_run() noexcept { return false; }
};

template <class Atomic>
using interleaved = Atomic;

#endif

}  // namespace bbtree::detail
