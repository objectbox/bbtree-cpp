// The sizes of design/02-field_study.md §6, and the scales cells run at: the
// Rust bench's `MapSize`, `MapShape` and `Scale` (`workload.rs`).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include "named.hpp"

namespace bbtree_bench {

// Full-size runs, or `--quick` ones for CI and development.
enum class scale {
  full,
  // 10x fewer entries, tiny maps and timed operations.
  quick,
};

template <>
struct enum_traits<scale> {
  static constexpr std::size_t shrink(scale s, std::size_t count) noexcept {
    return s == scale::full ? count : count / 10;
  }

  // The fewest operations a cell times. Small maps are rebuilt or reused
  // until they reach it; lookups on large maps stop at it, since a random
  // sample of the keys times as well as all of them.
  static constexpr std::size_t operation_budget(scale s) noexcept { return shrink(s, 1'000'000); }
};

// How many maps a cell builds, and how many entries each one holds.
class map_shape {
 public:
  // How many distinct keys many tiny maps draw their keys from, as JSON
  // objects of one kind share field names.
  static constexpr std::size_t vocabulary_size = 256;

  constexpr map_shape(std::size_t map_count, std::size_t entries_per_map) noexcept
      : map_count_(map_count), entries_per_map_(entries_per_map) {}

  constexpr std::size_t map_count() const noexcept { return map_count_; }
  constexpr std::size_t entries_per_map() const noexcept { return entries_per_map_; }

 private:
  std::size_t map_count_;
  std::size_t entries_per_map_;
};

// Many tiny maps alive at once, where the cost per map matters, or one map.
enum class map_size {
  // 1M maps of 4 entries: JSON objects (median 2 keys, 92% have <= 8).
  four_entry_maps,
  // 100k maps of 32 entries: JSON objects at p99.
  thirty_two_entry_maps,
  // About 1M maps, each holding the keys of one object of real JSON
  // documents: the documents' objects, repeated.
  real_json_objects,
  // One map of 1k entries, like cargo's per-package maps: rebuilt or reused
  // until the cell has timed enough operations.
  one_thousand,
  ten_thousand,
  one_million,
  // DRAM-resident: memtables and indexes.
  ten_million,
  // One map of every key of a dataset, but those held out for misses.
  all,
};

template <>
struct enum_traits<map_size> {
  static constexpr std::array all{
      map_size::four_entry_maps,   map_size::thirty_two_entry_maps,
      map_size::real_json_objects, map_size::one_thousand,
      map_size::ten_thousand,      map_size::one_million,
      map_size::ten_million,       map_size::all,
  };

  static constexpr std::string_view name(map_size size) noexcept {
    switch (size) {
      case map_size::four_entry_maps:
        return "1Mx4";
      case map_size::thirty_two_entry_maps:
        return "100kx32";
      case map_size::real_json_objects:
        return "1Mxreal";
      case map_size::one_thousand:
        return "1k";
      case map_size::ten_thousand:
        return "10k";
      case map_size::one_million:
        return "1M";
      case map_size::ten_million:
        return "10M";
      case map_size::all:
        return "all";
    }
    return "";
  }

  // The sizes whose shape is fixed, which are those of generated keys.
  static constexpr std::array fixed_shapes{
      map_size::four_entry_maps, map_size::thirty_two_entry_maps, map_size::one_thousand,
      map_size::ten_thousand,    map_size::one_million,           map_size::ten_million,
  };

  // How many maps `1Mxreal` builds, about: its JSON objects repeat until
  // there are at least this many.
  static constexpr std::size_t real_json_object_maps = 1'000'000;

  // The shape of the sizes that fix it; none for those whose dataset does.
  static constexpr std::optional<map_shape> fixed_shape(map_size size, scale s) noexcept {
    using scales = enum_traits<scale>;
    switch (size) {
      case map_size::four_entry_maps:
        return map_shape(scales::shrink(s, 1'000'000), 4);
      case map_size::thirty_two_entry_maps:
        return map_shape(scales::shrink(s, 100'000), 32);
      case map_size::one_thousand:
        return map_shape(1, scales::shrink(s, 1'000));
      case map_size::ten_thousand:
        return map_shape(1, scales::shrink(s, 10'000));
      case map_size::one_million:
        return map_shape(1, scales::shrink(s, 1'000'000));
      case map_size::ten_million:
        return map_shape(1, scales::shrink(s, 10'000'000));
      case map_size::real_json_objects:
      case map_size::all:
        return std::nullopt;
    }
    return std::nullopt;
  }

  static constexpr bool has_many_maps(map_size size) noexcept {
    return size == map_size::four_entry_maps || size == map_size::thirty_two_entry_maps ||
           size == map_size::real_json_objects;
  }
};

static_assert(
    std::ranges::all_of(enum_traits<map_size>::all,
                        [](map_size size) {
                          constexpr auto& fixed = enum_traits<map_size>::fixed_shapes;
                          return enum_traits<map_size>::fixed_shape(size, scale::full).has_value() ==
                                 (std::ranges::find(fixed, size) != fixed.end());
                        }),
    "the fixed shapes are the sizes with a shape");

}  // namespace bbtree_bench
