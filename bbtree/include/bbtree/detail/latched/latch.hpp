// Latches for a tree that threads share (09-concurrency.md, D54): a word in
// every node's header (`node_latch`), a wait that backs off (`backoff`), and
// the strategy that puts the word there (`latched`).
//
// One word serves both ways the tree is latched:
// - internal nodes, optimistically: a reader reads the version, reads the
//   node, and checks the version is unchanged; a writer takes the node by
//   compare-and-swap from the version it read, never waiting, and bumps the
//   version as it lets go (optimistic lock coupling);
// - leaves, pessimistically: shared for reads, exclusive for changes,
//   waiting with backoff. Letting go exclusively bumps the version too, so a
//   lookup can read a leaf optimistically as well (D57).
// The word, from the low bit: the exclusive bit, a 20-bit count of shared
// holders, a 43-bit version. A version is the word without its count.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

// Whether ThreadSanitizer instruments this build: then the racing reads tell
// it to ignore them, through the dynamic annotations its runtime provides.
#if defined(__SANITIZE_THREAD__)
#define BBTREE_TSAN_ANNOTATIONS 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define BBTREE_TSAN_ANNOTATIONS 1
#endif
#endif
#ifndef BBTREE_TSAN_ANNOTATIONS
#define BBTREE_TSAN_ANNOTATIONS 0
#endif

#if BBTREE_TSAN_ANNOTATIONS
extern "C" void AnnotateIgnoreReadsBegin(const char* file, int line);
extern "C" void AnnotateIgnoreReadsEnd(const char* file, int line);
#endif

namespace bbtree {

// A shared map's strategy: `S`'s inline keys, and a latch word in every node
// header. Nothing public names it.
template <class S>
struct latched {};

}  // namespace bbtree

namespace bbtree::detail {

// Waits that start with a short spin and then yield to the scheduler, as the
// holder may be preempted or running a user's closure (D3).
class backoff {
 public:
  void wait() noexcept {
    if (step_ < spin_steps) {
      for (unsigned spin = 0; spin < (1u << step_); ++spin) {
        spin_hint();
      }
      ++step_;
    } else {
      std::this_thread::yield();
    }
  }

 private:
  static constexpr unsigned spin_steps = 7;

  // Tells the CPU this is a spin-wait, as Rust's `core::hint::spin_loop`
  // does: `pause` on x86, `isb` on AArch64. It also keeps the compiler from
  // deleting the spin as an empty loop, which it would on other targets.
  static void spin_hint() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("isb" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
  }
  unsigned step_ = 0;
};

// What an optimistic reader saw of a node: its latch word without the shared
// count, unlatched. Equal versions of one node mean nothing changed it.
class latch_version {
 public:
  constexpr latch_version() noexcept = default;
  constexpr explicit latch_version(std::uint64_t word) noexcept : word_(word) {}
  constexpr std::uint64_t word() const noexcept { return word_; }
  friend constexpr bool operator==(latch_version, latch_version) = default;

 private:
  std::uint64_t word_ = 0;
};

// A node's latch word, at offset 8 of every latched page. Only atomics touch
// it; a fresh, zeroed page is unheld at version 0.
class node_latch {
 public:
  static constexpr std::uint64_t exclusive = 1;
  static constexpr std::uint64_t shared_unit = 1 << 1;
  static constexpr std::uint64_t shared_mask = ((std::uint64_t{1} << 20) - 1) << 1;
  static constexpr std::uint64_t version_unit = std::uint64_t{1} << 21;

  explicit node_latch(std::byte* page) noexcept
      : word_(*reinterpret_cast<std::uint64_t*>(page + latch_offset)) {}

  static constexpr std::size_t latch_offset = 8;

  // The node's version, once no thread holds it exclusively; waits until then.
  latch_version read_version() const noexcept {
    const std::uint64_t word = word_.load(std::memory_order_acquire);
    if ((word & exclusive) == 0) {
      return latch_version(word & ~shared_mask);
    }
    return read_version_contended();
  }

  // The node's version if nobody holds it exclusively now; none otherwise.
  bool try_read_version(latch_version& version) const noexcept {
    const std::uint64_t word = word_.load(std::memory_order_acquire);
    version = latch_version(word & ~shared_mask);
    return (word & exclusive) == 0;
  }

  // Whether the node is still at `version`, after the reads it vouches for;
  // the fence keeps those reads before the check.
  bool is_unchanged(latch_version version) const noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    return (word_.load(std::memory_order_relaxed) & ~shared_mask) == version.word();
  }

  // Takes the node exclusively if it is still at `version` with no shared
  // holder; fails at once otherwise.
  bool try_lock_at(latch_version version) noexcept {
    std::uint64_t expected = version.word();
    const bool locked = word_.compare_exchange_strong(expected, version.word() | exclusive,
                                                      std::memory_order_acquire, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    return locked;
  }

  // Lets go of the node, held exclusively since `version`, at the next one.
  void unlock_exclusive(latch_version version) noexcept {
    word_.store(version.word() + version_unit, std::memory_order_release);
  }

  // Takes the node exclusively once no thread holds it in either mode.
  latch_version lock_exclusive() noexcept {
    const std::uint64_t word = word_.load(std::memory_order_relaxed);
    if ((word & (exclusive | shared_mask)) == 0 && try_lock_at(latch_version(word))) {
      return latch_version(word);
    }
    return lock_exclusive_contended();
  }

  // Takes the node exclusively now if nobody holds it in either mode.
  bool try_lock_exclusive(latch_version& version) noexcept {
    const std::uint64_t word = word_.load(std::memory_order_relaxed);
    version = latch_version(word);
    return (word & (exclusive | shared_mask)) == 0 && try_lock_at(version);
  }

  void lock_shared() noexcept {
    const std::uint64_t word = word_.load(std::memory_order_relaxed);
    if ((word & exclusive) == 0 && try_add_shared_holder(word)) {
      return;
    }
    lock_shared_contended();
  }

  void unlock_shared() noexcept { word_.fetch_sub(shared_unit, std::memory_order_release); }

 private:
  bool try_add_shared_holder(std::uint64_t word) noexcept {
    return word_.compare_exchange_weak(word, word + shared_unit, std::memory_order_acquire,
                                       std::memory_order_relaxed);
  }

  [[gnu::cold, gnu::noinline]] latch_version read_version_contended() const noexcept {
    backoff wait;
    while (true) {
      wait.wait();
      const std::uint64_t word = word_.load(std::memory_order_acquire);
      if ((word & exclusive) == 0) {
        return latch_version(word & ~shared_mask);
      }
    }
  }

  [[gnu::cold, gnu::noinline]] latch_version lock_exclusive_contended() noexcept {
    backoff wait;
    while (true) {
      wait.wait();
      const std::uint64_t word = word_.load(std::memory_order_relaxed);
      if ((word & (exclusive | shared_mask)) == 0 && try_lock_at(latch_version(word))) {
        return latch_version(word);
      }
    }
  }

  [[gnu::cold, gnu::noinline]] void lock_shared_contended() noexcept {
    backoff wait;
    while (true) {
      const std::uint64_t word = word_.load(std::memory_order_relaxed);
      if ((word & exclusive) == 0 && try_add_shared_holder(word)) {
        return;
      }
      wait.wait();
    }
  }

  std::atomic_ref<std::uint64_t> word_;
};

// Tells ThreadSanitizer to ignore the reads between its construction and its
// end: the racing reads of optimistic lock coupling, which the version check
// after them vouches for or throws away (PORTING.md, P11).
class racing_reads {
 public:
#if BBTREE_TSAN_ANNOTATIONS
  racing_reads() noexcept { ::AnnotateIgnoreReadsBegin(__FILE__, __LINE__); }
  ~racing_reads() { ::AnnotateIgnoreReadsEnd(__FILE__, __LINE__); }
#else
  // User-provided, so that a guard that does nothing outside ThreadSanitizer
  // builds is not an unused variable.
  racing_reads() noexcept {}
  ~racing_reads() {}
#endif
  racing_reads(const racing_reads&) = delete;
  racing_reads& operator=(const racing_reads&) = delete;
};

// What an operation that runs a closure on an entry returns: the closure's
// result if the entry was there, or nothing; for a closure that returns
// nothing, whether the entry was there.
template <class R>
struct closure_result {
  using type = std::optional<R>;
  static type none() noexcept { return std::nullopt; }
  template <class F, class... Args>
  static type of(F& f, Args&&... args) {
    return type(f(std::forward<Args>(args)...));
  }
};

template <>
struct closure_result<void> {
  using type = bool;
  static type none() noexcept { return false; }
  template <class F, class... Args>
  static type of(F& f, Args&&... args) {
    f(std::forward<Args>(args)...);
    return true;
  }
};

}  // namespace bbtree::detail
