// The maps under test, by name: the Rust bench's `MapKind`
// (`map_adapters.rs`). The adapters themselves are in `map_adapters.hpp`;
// each lists the operations its map's API lacks, whose cells are reported as
// `n/a`.
#pragma once

#include <array>
#include <string_view>

#include "keys.hpp"
#include "named.hpp"
#include "operation.hpp"

namespace bbtree_bench {

// The names are the Rust bench's, so that the two benches' CSVs line up;
// `absl-btree` is the C++ bench's own.
enum class map_kind {
  // `bbtree::bbtree_map`, the map threads share, on one thread. Runs only
  // when `--map` names it.
  bbtree,
  // `bbtree::unsync::bbtree_map`, the map for one thread.
  unsync,
  // The Rust bench's `unsync::BBTreeMap` with the `WithInlineKey` strategy,
  // on integer keys only. In C++ inline keys come by specialization, not by
  // strategy (PORTING.md, P5), and the integers have theirs under the
  // default strategy, as they do under the Rust crate's `Auto`: the same map
  // as `bbtree-unsync`, kept under its name so that `--map` and the CSVs
  // line up (PORTING.md, P18). Runs only when `--map` names it.
  unsync_inline,
  // `bbtree::unsync::bbtree_map` with compact-string keys, on string keys
  // only, which the cell turns into compact strings before it times
  // anything (CPP_MAPS.md). The C++ bench's own: no Rust counterpart, so the
  // suites shared with the Rust bench leave them out.
  // `bbtree::compact_string`: 16 bytes, a 4-byte size, 12 in place.
  unsync_compact,
  // `basic_compact_string<std::uint16_t, 14>`: 16 bytes, a 2-byte size, 14
  // in place.
  unsync_compact_u16,
  // `bbtree::compact_string_24`: 24 bytes, a 4-byte size, 20 in place.
  unsync_compact_24,
  // `std::map`, the counterpart of Rust's `BTreeMap`.
  std_btree,
  // `absl::btree_map`: a B-tree of std::map's interface, the C++ bench's
  // extra baseline.
  absl_btree,
  // `ankerl::unordered_dense::map` with its default hash (wyhash), the
  // counterpart of hashbrown's `HashMap` with foldhash: a flat Swiss-table
  // rival (PORTING.md, P18).
  hashbrown,
  // `std::unordered_map` with `std::hash`, the counterpart of std's
  // `HashMap` (SipHash-1-3).
  std_hash,
};

template <>
struct enum_traits<map_kind> {
  static constexpr std::array all{
      map_kind::bbtree,
      map_kind::unsync,
      map_kind::unsync_inline,
      map_kind::unsync_compact,
      map_kind::unsync_compact_u16,
      map_kind::unsync_compact_24,
      map_kind::std_btree,
      map_kind::absl_btree,
      map_kind::hashbrown,
      map_kind::std_hash,
  };

  static constexpr std::string_view name(map_kind map) noexcept {
    switch (map) {
      case map_kind::bbtree:
        return "bbtree";
      case map_kind::unsync:
        return "bbtree-unsync";
      case map_kind::unsync_inline:
        return "bbtree-unsync-inline";
      case map_kind::unsync_compact:
        return "bbtree-unsync-compact";
      case map_kind::unsync_compact_u16:
        return "bbtree-unsync-compact-u16";
      case map_kind::unsync_compact_24:
        return "bbtree-unsync-compact-24";
      case map_kind::std_btree:
        return "std-btree";
      case map_kind::absl_btree:
        return "absl-btree";
      case map_kind::hashbrown:
        return "hashbrown";
      case map_kind::std_hash:
        return "std-hash";
    }
    return "";
  }

  // The column title in reports.
  static constexpr std::string_view title(map_kind map) noexcept {
    switch (map) {
      case map_kind::bbtree:
        return "bbtree";
      case map_kind::unsync:
        return "bbtree unsync";
      case map_kind::unsync_inline:
        return "bbtree unsync, inline keys";
      case map_kind::unsync_compact:
        return "bbtree unsync, compact_string";
      case map_kind::unsync_compact_u16:
        return "bbtree unsync, compact string, 2-byte size";
      case map_kind::unsync_compact_24:
        return "bbtree unsync, compact_string_24";
      case map_kind::std_btree:
        return "std::map";
      case map_kind::absl_btree:
        return "absl btree_map";
      case map_kind::hashbrown:
        return "ankerl unordered_dense";
      case map_kind::std_hash:
        return "std::unordered_map";
    }
    return "";
  }

  // Whether the map has the API `op` needs; read from its adapter
  // (`map_kind.cpp`).
  static bool supports(map_kind map, operation op) noexcept;

  // Whether the map runs on keys of `key`: `bbtree-unsync-inline` only on
  // integers, the compact-string maps only on strings.
  static constexpr bool supports_key(map_kind map, key_kind key) noexcept {
    if (map == map_kind::unsync_inline) {
      return enum_traits<key_kind>::is_inline_keyed_integer(key);
    }
    return !has_compact_keys(map) || !enum_traits<key_kind>::is_inline_keyed_integer(key);
  }

  // Whether the map's keys are compact strings.
  static constexpr bool has_compact_keys(map_kind map) noexcept {
    return map == map_kind::unsync_compact || map == map_kind::unsync_compact_u16 ||
           map == map_kind::unsync_compact_24;
  }

  // Whether the map runs only when `--map` names it.
  static constexpr bool is_opt_in(map_kind map) noexcept {
    return map == map_kind::unsync_inline || map == map_kind::bbtree;
  }
};

}  // namespace bbtree_bench
