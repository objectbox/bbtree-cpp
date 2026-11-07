// What the examples share: seeded random numbers, a map of random `u64`
// entries, and the time a lookup takes, of keys that hit and keys that miss
// (the Rust crate's `examples/timed_lookups/mod.rs`).
#pragma once

#include <bbtree/unsync/map.hpp>

#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bbtree_mmap_examples {

// A xorshift64* generator, seeded: the same keys and lookups every run.
class seeded_numbers {
 public:
  std::uint64_t next() noexcept {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545'F491'4F6C'DD1DULL;
  }

  // The next number, below `bound`.
  std::size_t below(std::size_t bound) noexcept { return static_cast<std::size_t>(next() % bound); }

 private:
  std::uint64_t state_ = 0x5eed;
};

// Keys to look up: some that all hit, and as many that all miss.
template <class Q>
class timed_lookups {
 public:
  timed_lookups(std::vector<Q> hits, std::vector<Q> misses)
      : hits_(std::move(hits)), misses_(std::move(misses)) {}

  // A map of `count` random `u64` entries, of even keys, and `count` lookups
  // of its keys and of odd ones.
  static std::pair<bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t>, timed_lookups> of_u64_map(
      std::size_t count, seeded_numbers& numbers)
    requires std::same_as<Q, std::uint64_t>
  {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> entries(count);
    for (auto& [key, value] : entries) {
      key = numbers.next() & ~std::uint64_t{1};
      value = std::rotl(key, 17);
    }
    bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map(entries.begin(), entries.end());
    std::vector<std::uint64_t> hits(count);
    for (auto& hit : hits) {
      hit = entries[numbers.below(count)].first;
    }
    std::vector<std::uint64_t> misses(count);
    for (auto& miss : misses) {
      miss = numbers.next() | 1;
    }
    return {std::move(map), timed_lookups(std::move(hits), std::move(misses))};
  }

  // Times `look_up` over the hits, then over the misses, and prints the time
  // a lookup of each took. `look_up(key)` returns a `std::optional<std::uint64_t>`.
  template <class LookUp>
  void report(std::string_view name, LookUp&& look_up) const {
    const auto [hit_ns, found] = time(hits_, look_up);
    const auto [miss_ns, missed] = time(misses_, look_up);
    if (found != hits_.size() || missed != 0) {
      throw std::runtime_error(std::string(name) + ": lookups found the wrong keys");
    }
    std::printf("%-32.*s hit %6.1f ns   miss %6.1f ns\n", static_cast<int>(name.size()), name.data(), hit_ns,
                miss_ns);
  }

 private:
  // One pass of `look_up` over `keys`, after an untimed pass over a tenth of
  // them: nanoseconds per lookup, and the hits.
  template <class LookUp>
  static std::pair<double, std::size_t> time(const std::vector<Q>& keys, LookUp& look_up) {
    std::size_t warm = 0;
    for (std::size_t index = 0; index < keys.size() / 10; ++index) {
      if (look_up(keys[index]).has_value()) {
        ++warm;
      }
    }
    keep(warm);
    std::size_t hits = 0;
    std::uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const Q& key : keys) {
      if (const std::optional<std::uint64_t> value = look_up(*opaque(&key))) {
        ++hits;
        sum += *value;
      }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    keep(sum);
    const double ns = std::chrono::duration<double, std::nano>(elapsed).count();
    return {ns / static_cast<double>(keys.size()), hits};
  }

  // What `std::hint::black_box` does: the compiler must assume `value` is
  // read here.
  template <class T>
  static void keep(const T& value) noexcept {
    asm volatile("" : : "r,m"(value) : "memory");
  }

  // `pointer`, as something the compiler can't see through, so that the
  // lookups aren't hoisted or merged.
  static const Q* opaque(const Q* pointer) noexcept {
    asm volatile("" : "+r"(pointer) : : "memory");
    return pointer;
  }

  std::vector<Q> hits_;
  std::vector<Q> misses_;
};

}  // namespace bbtree_mmap_examples
