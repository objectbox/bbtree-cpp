// The operations of design/02-field_study.md §6, on one map
// (`single_map.hpp`) or on many tiny maps (`many_maps.hpp`): the Rust bench's
// `Operation` (`operations.rs`).
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "map_size.hpp"
#include "named.hpp"

namespace bbtree_bench {

// What a cell does. Time is reported per operation, which is one call of the
// map's API unless noted.
enum class operation {
  // Inserts every key into an empty map, in random order.
  random_insert,
  // Inserts every key into an empty map, in ascending order.
  sequential_insert,
  // Looks up keys that are in the map.
  get_hit,
  // Looks up keys that aren't in the map, which fall between keys that are.
  get_miss,
  // Visits every entry, in key order for ordered maps. Per entry.
  iterate,
  // Visits 64 entries in key order from a random key in the map. Per scan.
  range64,
  // Removes every key, in random order.
  random_remove,
  // The counter-increment pattern, `++map[key]`, for every key twice in
  // random order: half inserts, half updates.
  entry_upsert,
  // Pops every entry, smallest first: a drain, as timers and queues do.
  pop_first,
  // The range constructor, from entries sorted by key. Per entry.
  bulk_build,
  // Builds many tiny maps, all kept alive, inserting each map's keys in its
  // own order: random for generated maps, document order for JSON objects.
  // Per insert.
  build_many,
  // Looks up every key of every tiny map, visiting the maps in random order.
  get_many,
  // Looks up, in every tiny map, as many keys that it lacks as it holds,
  // visiting the maps in random order. The keys are the other maps', so they
  // mix like those of `get_many`.
  get_many_miss,
};

template <>
struct enum_traits<operation> {
  static constexpr std::array all{
      operation::random_insert, operation::sequential_insert, operation::get_hit,
      operation::get_miss,      operation::iterate,           operation::range64,
      operation::random_remove, operation::entry_upsert,      operation::pop_first,
      operation::bulk_build,    operation::build_many,        operation::get_many,
      operation::get_many_miss,
  };

  static constexpr std::string_view name(operation op) noexcept {
    switch (op) {
      case operation::random_insert:
        return "random_insert";
      case operation::sequential_insert:
        return "sequential_insert";
      case operation::get_hit:
        return "get_hit";
      case operation::get_miss:
        return "get_miss";
      case operation::iterate:
        return "iterate";
      case operation::range64:
        return "range64";
      case operation::random_remove:
        return "random_remove";
      case operation::entry_upsert:
        return "entry_upsert";
      case operation::pop_first:
        return "pop_first";
      case operation::bulk_build:
        return "bulk_build";
      case operation::build_many:
        return "build_many";
      case operation::get_many:
        return "get_many";
      case operation::get_many_miss:
        return "get_many_miss";
    }
    return "";
  }

  // The operations on one map, all of which the small and ci suites run.
  static constexpr std::array on_one_map{
      operation::random_insert, operation::sequential_insert, operation::get_hit,
      operation::get_miss,      operation::iterate,           operation::range64,
      operation::random_remove, operation::entry_upsert,      operation::pop_first,
      operation::bulk_build,
  };

  // The operations on many tiny maps: build them, then read them as a
  // deserializer's caller or a serializer would.
  static constexpr std::array on_many_maps{
      operation::build_many,
      operation::get_many,
      operation::get_many_miss,
      operation::iterate,
  };

  static constexpr std::size_t range_scan_length = 64;

  static constexpr bool applies_to(operation op, map_size size) noexcept {
    if (enum_traits<map_size>::has_many_maps(size)) {
      return named<operation>::listed(on_many_maps, op);
    }
    return named<operation>::listed(on_one_map, op);
  }

  // Whether the operation builds maps from inputs made beforehand: the
  // operations whose maps memory mode measures.
  static constexpr bool builds_maps(operation op) noexcept {
    return op == operation::random_insert || op == operation::sequential_insert ||
           op == operation::bulk_build || op == operation::build_many;
  }
};

}  // namespace bbtree_bench
