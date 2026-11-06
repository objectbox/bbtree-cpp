// Deterministic pseudo-random numbers, so that every map in a cell sees the
// same keys, values and access orders: the Rust bench's `random.rs`, draw for
// draw, so that both benches measure the same key sets and orders.
#pragma once

#include <bbtree/inline_key.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bbtree_bench {

// What a random sequence is for. Each purpose has its own seed, so that,
// say, the lookup order doesn't shift when key generation draws one more
// number. The seed is the variant's position: new ones go last.
enum class random_stream : std::uint64_t {
  key_content,
  insertion_order,
  lookup_order,
  removal_order,
  map_order,
  // Which keys of a dataset a map of less than all of them holds.
  dataset_sample,
  // Which keys lookups that miss tiny maps probe with.
  absent_keys,
};

// SplitMix64 (Steele, Lea and Flood, 2014): small, fast, and random enough
// for benchmark data.
class seeded_rng {
 public:
  explicit seeded_rng(random_stream stream) noexcept : state_(mix(static_cast<std::uint64_t>(stream))) {}

  std::uint64_t next_u64() noexcept {
    state_ += golden_gamma;
    return mix(state_);
  }

  // A number in `0..bound`, by Lemire's multiply-and-shift; the bias is
  // below 2^-32 for any bound the benchmark uses.
  std::size_t below(std::size_t bound) noexcept {
    const bbtree::uint128 product = static_cast<bbtree::uint128>(next_u64()) * bound;
    return static_cast<std::size_t>(product >> 64);
  }

  // A number in `first..=last`.
  std::size_t in_range(std::size_t first, std::size_t last) noexcept {
    return first + below(last - first + 1);
  }

  // One of `table`'s values, each as likely as its weight.
  template <class T, std::size_t N>
  T weighted(const std::array<std::pair<T, std::uint32_t>, N>& table) noexcept {
    std::uint32_t total = 0;
    for (const auto& [value, weight] : table) {
      total += weight;
    }
    auto ticket = static_cast<std::uint32_t>(below(total));
    for (const auto& [value, weight] : table) {
      if (ticket < weight) {
        return value;
      }
      ticket -= weight;
    }
    return table.back().first;  // unreachable: the ticket is below the total weight
  }

  // `length` characters drawn from `alphabet`, which must be ASCII.
  std::string ascii_text(std::size_t length, std::string_view alphabet) {
    std::string text;
    text.reserve(length);
    for (std::size_t drawn = 0; drawn < length; ++drawn) {
      text.push_back(alphabet[below(alphabet.size())]);
    }
    return text;
  }

  // Fisher-Yates. The Rust bench shuffles many items on all cores, by their
  // indices, with the same result and the same draws as this serial
  // shuffle; here it is serial for every size.
  template <class T>
  void shuffle(std::vector<T>& items) noexcept {
    for (std::size_t index = items.size(); index-- > 1;) {
      std::swap(items[index], items[below(index + 1)]);
    }
  }

  // Moves `count` distinct items, chosen at random, to the front of
  // `items`: the first `count` steps of Fisher-Yates.
  template <class T>
  void choose_to_front(std::span<T> items, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
      const std::size_t chosen = index + below(items.size() - index);
      std::swap(items[index], items[chosen]);
    }
  }

  // The indices of the items that `choose_to_front` would move to the front
  // of `item_count` items, in its order.
  std::vector<std::size_t> choose_indices(std::size_t item_count, std::size_t count) {
    std::vector<std::size_t> indices(item_count);
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    choose_to_front(std::span<std::size_t>(indices), count);
    indices.resize(count);
    return indices;
  }

  // SplitMix64's output function. It is a bijection on 64-bit numbers, so
  // distinct inputs give distinct, random-looking outputs.
  static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
    x = (x ^ (x >> 30)) * 0xBF58'476D'1CE4'E5B9;
    x = (x ^ (x >> 27)) * 0x94D0'49BB'1331'11EB;
    return x ^ (x >> 31);
  }

 private:
  static constexpr std::uint64_t golden_gamma = 0x9E37'79B9'7F4A'7C15;

  std::uint64_t state_;
};

}  // namespace bbtree_bench
