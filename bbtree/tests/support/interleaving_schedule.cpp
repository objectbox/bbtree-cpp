#include "interleaving_schedule.hpp"

#include <cassert>
#include <cstdio>
#include <thread>
#include <utility>

#if BBTREE_TSAN_ANNOTATIONS
extern "C" void AnnotateIgnoreWritesBegin(const char* file, int line);
extern "C" void AnnotateIgnoreWritesEnd(const char* file, int line);
extern "C" void AnnotateIgnoreSyncBegin(const char* file, int line);
extern "C" void AnnotateIgnoreSyncEnd(const char* file, int line);
#endif

namespace bbtree_test {

namespace {

// Hides the scheduler from ThreadSanitizer while it lives: what it reads and
// writes, and the mutex and condition variables the turn goes through, which
// would otherwise order one thread's accesses before the next thread's.
class unobserved {
 public:
#if BBTREE_TSAN_ANNOTATIONS
  unobserved() noexcept {
    ::AnnotateIgnoreReadsBegin(__FILE__, __LINE__);
    ::AnnotateIgnoreWritesBegin(__FILE__, __LINE__);
    ::AnnotateIgnoreSyncBegin(__FILE__, __LINE__);
  }
  ~unobserved() {
    ::AnnotateIgnoreSyncEnd(__FILE__, __LINE__);
    ::AnnotateIgnoreWritesEnd(__FILE__, __LINE__);
    ::AnnotateIgnoreReadsEnd(__FILE__, __LINE__);
  }
#else
  unobserved() noexcept {}
  ~unobserved() {}
#endif
  unobserved(const unobserved&) = delete;
  unobserved& operator=(const unobserved&) = delete;
};

}  // namespace

interleaving_schedule::interleaving_schedule(std::uint64_t seed)
    : seed_(seed), rng_(seed), switch_one_in_(std::uint64_t{2} << (rng_() % 8)) {}

interleaving_schedule::turn*& interleaving_schedule::current() noexcept {
  thread_local turn* mine = nullptr;
  return mine;
}

void interleaving_schedule::run(std::vector<std::function<void()>> bodies) {
  assert(!bodies.empty());
  turns_.clear();
  for (std::size_t index = 0; index < bodies.size(); ++index) {
    turns_.push_back(std::make_unique<turn>(*this, index));
  }
  running_.store(rng_() % bodies.size(), std::memory_order_relaxed);
  // Starting and joining the threads stays in ThreadSanitizer's sight: a
  // thread may read what the test set up, and the test what the threads did.
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < bodies.size(); ++index) {
    threads.emplace_back([this, index, body = std::move(bodies[index])] {
      turn& mine = *turns_[index];
      current() = &mine;
      wait_for_turn(mine);
      body();
      finish(mine);
      current() = nullptr;
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

void interleaving_schedule::expect(bool holds, std::string_view what) {
  if (holds) {
    return;
  }
  // The running thread's, as all the schedule's state is.
  const unobserved hidden;
  if (failure_.empty()) {
    failure_ = what;
  }
}

void interleaving_schedule::switch_point() noexcept {
  if (turn* const mine = current()) {
    mine->schedule.pass(*mine, false);
  }
}

bool interleaving_schedule::let_another_run() noexcept {
  turn* const mine = current();
  if (mine == nullptr) {
    return false;
  }
  mine->schedule.pass(*mine, true);
  return true;
}

void interleaving_schedule::wait_for_turn(turn& mine) {
  const unobserved hidden;
  for (unsigned spin = 0; spin < 2'000; ++spin) {
    if (running_.load(std::memory_order_acquire) == mine.index) {
      return;
    }
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#endif
  }
  std::unique_lock<std::mutex> lock(mutex_);
  mine.granted.wait(lock, [&] { return running_.load(std::memory_order_relaxed) == mine.index; });
}

void interleaving_schedule::pass(turn& from, bool must_switch) noexcept {
  const unobserved hidden;
  // Only the running thread is here: the turn's own state needs no lock.
  if (++steps_ > max_steps) {
    std::fprintf(stderr, "interleaving schedule %llu: no progress in %llu switch points\n",
                 static_cast<unsigned long long>(seed_), static_cast<unsigned long long>(max_steps));
    std::abort();
  }
  if (!must_switch && rng_() % switch_one_in_ != 0) {
    return;
  }
  if (hand_over(from)) {
    wait_for_turn(from);
  }
}

void interleaving_schedule::finish(turn& done) {
  const unobserved hidden;
  done.finished = true;
  hand_over(done);
}

bool interleaving_schedule::hand_over(turn& from) {
  std::size_t others = 0;
  for (const auto& other : turns_) {
    if (other.get() != &from && !other->finished) {
      ++others;
    }
  }
  if (others == 0) {
    return false;
  }
  std::size_t pick = rng_() % others;
  for (const auto& other : turns_) {
    if (other.get() == &from || other->finished) {
      continue;
    }
    if (pick-- == 0) {
      trace_ = (trace_ ^ other->index) * 0x100000001b3ull + steps_;
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        running_.store(other->index, std::memory_order_release);
      }
      other->granted.notify_one();
      return true;
    }
  }
  return false;
}

}  // namespace bbtree_test

// The latched tree's switch points, on the calling thread's schedule.
void bbtree::detail::interleaving::switch_point() noexcept {
  bbtree_test::interleaving_schedule::switch_point();
}

bool bbtree::detail::interleaving::let_another_run() noexcept {
  return bbtree_test::interleaving_schedule::let_another_run();
}
