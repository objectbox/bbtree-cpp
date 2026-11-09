// Value kinds (design/02-field_study.md §6) and their deterministic
// generators: the Rust bench's `values.rs`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "named.hpp"
#include "random.hpp"

namespace bbtree_bench {

enum class value_kind {
  // `std::uint64_t`: counters, IDs.
  u64,
  // `std::array<std::uint64_t, 4>`: a 32-byte struct, `serde_json::Value`-sized.
  struct32,
};

template <>
struct enum_traits<value_kind> {
  static constexpr std::array all{value_kind::u64, value_kind::struct32};

  static constexpr std::string_view name(value_kind kind) noexcept {
    return kind == value_kind::u64 ? "u64" : "struct32";
  }
};

using struct32 = std::array<std::uint64_t, 4>;

// What the benchmark needs from a value type: the Rust bench's `BenchValue`.
template <class V>
struct value_traits;

template <>
struct value_traits<std::uint64_t> {
  // A cheap read of the value, folded into checksums so that the optimizer
  // can't skip visiting it.
  static std::uint64_t fingerprint(std::uint64_t value) noexcept { return value; }

  // The update of the counter-increment pattern (`entry_upsert`): `+= 1` on
  // a number, or on a struct's first field.
  static void bump(std::uint64_t& value) noexcept { ++value; }
};

template <>
struct value_traits<struct32> {
  static std::uint64_t fingerprint(const struct32& value) noexcept { return value[0]; }
  static void bump(struct32& value) noexcept { ++value[0]; }
};

// Makes the value of each entry from its index, on any thread.
template <class V>
class value_maker {
 public:
  V value(std::uint64_t index) const noexcept { return make_(index); }

  static value_maker counters() noexcept
    requires std::is_same_v<V, std::uint64_t>
  {
    return value_maker([](std::uint64_t index) noexcept { return index; });
  }

  static value_maker structs() noexcept
    requires std::is_same_v<V, struct32>
  {
    return value_maker(
        [](std::uint64_t index) noexcept { return struct32{index, ~index, seeded_rng::mix(index), 0}; });
  }

 private:
  using make_function = V (*)(std::uint64_t) noexcept;

  explicit value_maker(make_function make) noexcept : make_(make) {}

  make_function make_;
};

}  // namespace bbtree_bench
