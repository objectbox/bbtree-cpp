// Keys whose order lies: comparisons that are not transitive, and inline keys
// that disagree with the order (the Rust crate's `user_code_panics` target,
// whose panics have no counterpart here: user code must not throw, and where
// keys move it would end the program, D48). A lying order gives wrong answers,
// never undefined behavior: this target checks that under AddressSanitizer,
// and not the answers.
#pragma once

#include "fuzz_input.hpp"

#include <bbtree/unsync/map.hpp>

#include <compare>
#include <cstdint>

namespace bbtree_fuzz {

// Ordered rock-paper-scissors: a < b when (b - a) % 3 == 1, and otherwise by
// value, which is no order at all.
struct lying_key {
  std::uint32_t value;
  friend bool operator<(const lying_key& a, const lying_key& b) noexcept {
    const std::uint32_t step = (b.value - a.value) % 3;
    return step == 1 || (step == 0 && a.value < b.value);
  }
  friend bool operator==(const lying_key& a, const lying_key& b) noexcept { return a.value == b.value; }
};

// A key whose inline keys come from a hash, disagreeing with its order.
struct lying_window_key {
  std::uint64_t value;
  friend auto operator<=>(const lying_window_key&, const lying_window_key&) = default;
};

class hostile_keys {
 public:
  static void run(std::span<const std::uint8_t> bytes) {
    fuzz_input in(bytes);
    bbtree::unsync::bbtree_map<lying_key, std::uint64_t> lying;
    bbtree::unsync::bbtree_map<lying_window_key, std::uint64_t> windows;
    for (int step = 0; step < 4096 && !in.done(); ++step) {
      const auto value = static_cast<std::uint32_t>(in.below(3000));
      switch (in.byte() % 5) {
        case 0:
        case 1:
          lying.insert_or_assign(lying_key{value}, value);
          windows.insert_or_assign(lying_window_key{value}, value);
          break;
        case 2:
          lying.erase(lying_key{value});
          windows.erase(lying_window_key{value});
          break;
        case 3: {
          std::size_t visited = 0;
          for (auto it = lying.begin(); it != lying.end() && visited < 10'000; ++it) {
            ++visited;
          }
          for (auto it = windows.lower_bound(lying_window_key{value});
               it != windows.end() && visited < 20'000; ++it) {
            ++visited;
          }
          break;
        }
        default:
          (void)lying.find(lying_key{value});
          (void)windows.find(lying_window_key{value});
      }
    }
  }
};

}  // namespace bbtree_fuzz

template <>
struct bbtree::inline_key<bbtree_fuzz::lying_window_key> {
  static bbtree::inline_word at(const bbtree_fuzz::lying_window_key& key, std::size_t offset) noexcept {
    return (key.value * 0x9e3779b97f4a7c15u) >> (offset % 8);
  }
};
