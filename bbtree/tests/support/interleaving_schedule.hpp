// The scheduler behind the latched tree's switch points
// (`bbtree::detail::interleaving`, built with `BBTREE_INTERLEAVING`): what
// Shuttle is to the Rust crate's `map/shuttle_tests.rs`.
//
// `interleaving_schedule::run` runs a test's threads one at a time. The
// running thread goes on until a switch point, where the schedule's seed
// decides whether another thread runs next, and which; a thread that waits
// for another (`backoff`) always lets another run. So a schedule is a
// function of its seed and of what the threads do, and a seed that failed
// fails again: `interleaving_exploration` runs a test under many seeds and
// names the one that failed, and `BBTREE_INTERLEAVING_SEED=<seed>` in the
// environment runs that one alone.
//
// How often a schedule switches is drawn from its seed too, from every
// second switch point to one in 256: the rare switches let one thread run a
// whole split or merge while another sits between two of its reads, which
// is where the tree's races are; the frequent ones interleave the steps of
// two changes.
//
// ThreadSanitizer sees none of the scheduler: not its accesses, and not its
// handing over of the turn, which would order every access of one thread
// before the next thread's and hide every race. So under `--preset tsan` a
// schedule reports the data races real threads would have in the same
// interleaving, and the test's threads must share only what real threads
// could: atomics, or what a latch of the map covers. Their expectations go
// through `interleaving_schedule::expect`, as Catch2's assertions are not
// for threads.
//
// The switch points take no argument, so a thread finds its schedule through
// one thread-local pointer, the only static state here.
#pragma once

#include <bbtree/detail/latched/latch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace bbtree_test {

class interleaving_schedule {
 public:
  explicit interleaving_schedule(std::uint64_t seed);
  interleaving_schedule(const interleaving_schedule&) = delete;
  interleaving_schedule& operator=(const interleaving_schedule&) = delete;

  // Runs each body on a thread of its own, one thread at a time, until all
  // returned. A body throws nothing.
  void run(std::vector<std::function<void()>> bodies);

  // A thread's expectation: the first that fails is the schedule's failure.
  void expect(bool holds, std::string_view what);
  // What failed first, or nothing.
  const std::string& failure() const noexcept { return failure_; }

  std::uint64_t seed() const noexcept { return seed_; }
  // A number below `bound` drawn from the seed, for a test whose setup
  // varies by schedule; outside `run`.
  std::uint64_t draw(std::uint64_t bound) { return rng_() % bound; }
  // The switch points passed, and a hash of which thread ran after each
  // handing over: equal for equal seeds.
  std::uint64_t steps() const noexcept { return steps_; }
  std::uint64_t trace() const noexcept { return trace_; }

  // What `bbtree::detail::interleaving` calls on the calling thread's
  // schedule; nothing on a thread no schedule runs.
  static void switch_point() noexcept;
  static bool let_another_run() noexcept;

 private:
  // One of the schedule's threads, and where it sleeps until its turn.
  struct turn {
    turn(interleaving_schedule& owner, std::size_t at) noexcept : schedule(owner), index(at) {}

    interleaving_schedule& schedule;
    std::size_t index;
    std::condition_variable granted;
    bool finished = false;
  };

  // A schedule that passes this many switch points makes no progress: threads
  // that wait for each other forever.
  static constexpr std::uint64_t max_steps = 10'000'000;

  static turn*& current() noexcept;
  // Returns once it is `mine`'s turn: it spins shortly first, as a thread
  // that just ran is often next, then sleeps.
  void wait_for_turn(turn& mine);
  void pass(turn& from, bool must_switch) noexcept;
  void finish(turn& done);
  // Gives the turn to a thread other than `from` that has not finished,
  // drawn from the seed; false if there is none.
  bool hand_over(turn& from);

  std::uint64_t seed_;
  std::mt19937_64 rng_;
  std::uint64_t switch_one_in_;
  std::mutex mutex_;
  std::vector<std::unique_ptr<turn>> turns_;
  // Whose turn it is: written under the mutex, which a sleeper holds while
  // it checks, and read without it by a thread that spins.
  std::atomic<std::size_t> running_{0};
  std::uint64_t steps_ = 0;
  std::uint64_t trace_ = 0;
  std::string failure_;
};

// Runs a test under `schedules` seeds, each on a schedule of its own, and
// fails with the seed of the first schedule in which an expectation failed.
struct interleaving_exploration {
  std::size_t schedules;

  // Fewer schedules where a step costs many times a release build's: under
  // ThreadSanitizer and without optimization.
  static constexpr std::size_t scaled(std::size_t schedules) noexcept {
#if BBTREE_TSAN_ANNOTATIONS || !defined(__OPTIMIZE__)
    return schedules / 4;
#else
    return schedules;
#endif
  }

  template <class Test>
  void run(Test&& test) const {
    std::uint64_t first = 0;
    std::size_t count = scaled(schedules);
    if (const char* replayed = std::getenv("BBTREE_INTERLEAVING_SEED")) {
      first = std::strtoull(replayed, nullptr, 10);
      count = 1;
    }
    for (std::uint64_t seed = first; seed < first + count; ++seed) {
      interleaving_schedule schedule(seed);
      INFO("schedule seed " << seed << " (BBTREE_INTERLEAVING_SEED=" << seed << " runs it alone)");
      test(schedule);
      REQUIRE(schedule.failure() == "");
    }
  }
};

}  // namespace bbtree_test
