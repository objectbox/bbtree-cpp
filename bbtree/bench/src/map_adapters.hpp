// The maps under test, behind one interface: the Rust bench's `MapUnderTest`
// and its impls (`map_adapters.rs`).
//
// An adapter has the API every map has (`entry_count`, `insert`, and `get`
// or `lookup_fingerprint`), and the operations some maps lack, which it
// lists in `unsupported`: those never run, and their members say so if
// called. `lends_references` says whether lookups read through `get`, which
// hands out a pointer to the value, or, for the map threads share, through
// `lookup_fingerprint`, which reads the value under the leaf's latch.
//
// Every map with std::map's interface shares one adapter, `std_interface_map`;
// the map threads share has its own, `counted_bbtree`.
#pragma once

#include <absl/container/btree_map.h>
#include <ankerl/unordered_dense.h>
#include <bbtree/map.hpp>
#include <bbtree/pooled_allocator.hpp>
#include <bbtree/unsync/map.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "keys.hpp"
#include "named.hpp"
#include "operation.hpp"
#include "values.hpp"

namespace bbtree_bench {

// The entries a build consumes, in its order.
template <class K, class V>
using entry_list = std::vector<std::pair<K, V>>;

// What the members of operations an adapter lists as unsupported do, which
// is never to run.
class unsupported_operation {
 public:
  [[noreturn]] static void reached(operation op) {
    throw bench_error(
        std::format("{} is listed as unsupported, so it never runs", named<operation>::name(op)));
  }
};

// The checksum of a visited entry, which reads its key and its value.
template <class K, class V>
struct entry_checksum {
  static std::uint64_t of(const K& key, const V& value) noexcept {
    return key_traits<K>::fingerprint(key) ^ value_traits<V>::fingerprint(value);
  }
};

// A map with std::map's interface: bbtree's unsync map, `std::map`,
// `absl::btree_map`, and, unordered, the hash maps. Hash maps have no order:
// no range scans and no `pop_first`.
template <class Map, bool Ordered>
class std_interface_map {
  using K = typename Map::key_type;
  using V = typename Map::mapped_type;
  using checksum = entry_checksum<K, V>;

 public:
  static constexpr std::span<const operation> unsupported() noexcept {
    if constexpr (Ordered) {
      return {};
    } else {
      return unordered_lacks;
    }
  }

  static constexpr bool lends_references = true;

  std::size_t entry_count() const noexcept { return map_.size(); }

  // Inserts, replacing the value of a key the map holds, as Rust's `insert`.
  void insert(K key, V value) { map_.insert_or_assign(std::move(key), std::move(value)); }

  const V* get(const K& key) const noexcept {
    if constexpr (requires { map_.get(key); }) {
      return map_.get(key);
    } else {
      const auto found = map_.find(key);
      return found == map_.end() ? nullptr : &found->second;
    }
  }

  std::optional<std::uint64_t> lookup_fingerprint(const K& key) const noexcept {
    const V* value = get(key);
    return value ? std::optional(value_traits<V>::fingerprint(*value)) : std::nullopt;
  }

  // Visits every entry, in key order for ordered maps, and returns their
  // checksum.
  // bbtree's map folds a leaf at a time, as the Rust crate's iterators do in
  // the `fold` that the Rust bench's checksum is; the other maps visit
  // through their iterators.
  std::uint64_t checksum_all() const noexcept {
    const auto add_entry = [](std::uint64_t sum, const K& key, const V& value) {
      return sum + checksum::of(key, value);
    };
    if constexpr (requires { map_.fold(std::uint64_t{0}, add_entry); }) {
      return map_.fold(std::uint64_t{0}, add_entry);
    } else {
      std::uint64_t sum = 0;
      const auto add = [&sum](const K& key, const V& value) { sum += checksum::of(key, value); };
      for (auto&& [key, value] : map_) {
        add(key, value);
      }
      return sum;
    }
  }

  // Visits up to `count` entries in key order, from the first key at or
  // after `start`, and returns their checksum.
  std::uint64_t checksum_range(const K& start, std::size_t count) const {
    if constexpr (Ordered) {
      std::uint64_t sum = 0;
      auto at = map_.lower_bound(start);
      for (std::size_t visited = 0; visited < count && at != map_.end(); ++visited, ++at) {
        const auto& [key, value] = *at;
        sum += checksum::of(key, value);
      }
      return sum;
    } else {
      unsupported_operation::reached(operation::range64);
    }
  }

  bool remove(const K& key) noexcept { return map_.erase(key) == 1; }

  // The counter-increment pattern: `++map[key]`, as Rust's
  // `*map.entry(key).or_default() += 1`.
  void bump_count(K key) { value_traits<V>::bump(map_[std::move(key)]); }

  std::optional<std::pair<K, V>> pop_first() {
    if constexpr (!Ordered) {
      unsupported_operation::reached(operation::pop_first);
    } else if constexpr (requires { map_.pop_first(); }) {
      return map_.pop_first();
    } else {
      if (map_.empty()) {
        return std::nullopt;
      }
      auto node = map_.extract(map_.begin());
      return std::pair<K, V>(std::move(node.key()), std::move(node.mapped()));
    }
  }

  // The range constructor, from entries sorted by key: Rust's
  // `FromIterator`.
  static std_interface_map from_sorted_entries(entry_list<K, V> entries) {
    std_interface_map built;
    built.map_ = Map(std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
    return built;
  }

 private:
  static constexpr std::array unordered_lacks{operation::range64, operation::pop_first};

  Map map_;
};

template <class K, class V>
using unsync_bbtree = std_interface_map<bbtree::unsync::bbtree_map<K, V>, true>;

// `bbtree::unsync::bbtree_map` on `bbtree::pooled_allocator`: its pages packed
// into chunks (PORTING.md, P32).
template <class K, class V>
using pooled_unsync_bbtree =
    std_interface_map<bbtree::unsync::bbtree_map<K, V, std::less<K>, bbtree::pooled_allocator>, true>;

template <class K, class V>
using std_btree = std_interface_map<std::map<K, V>, true>;

template <class K, class V>
using absl_btree = std_interface_map<absl::btree_map<K, V>, true>;

template <class K, class V>
using unordered_dense_hash = std_interface_map<ankerl::unordered_dense::map<K, V>, false>;

template <class K, class V>
using std_hash = std_interface_map<std::unordered_map<K, V>, false>;

// `bbtree::bbtree_map`, and the count of entries it has taken, which the map
// itself doesn't keep: a shared counter would cost every insert an atomic.
// Lookups read through a closure, as the map hands out no references to
// threads that share it; iterating, through a scan.
template <class K, class V>
class counted_bbtree {
  using checksum = entry_checksum<K, V>;

 public:
  // `bbtree::bbtree_map` has no entry API, no `pop_first`, and builds only
  // by inserts.
  static constexpr std::span<const operation> unsupported() noexcept { return lacking; }

  static constexpr bool lends_references = false;

  std::size_t entry_count() const noexcept { return entries_; }

  void insert(K key, V value) {
    if (!map_.insert_or_assign(std::move(key), std::move(value))) {
      ++entries_;
    }
  }

  const V* get(const K&) const { unsupported_operation::reached(operation::get_hit); }

  std::optional<std::uint64_t> lookup_fingerprint(const K& key) const {
    return map_.read(key, [](const K&, const V& value) { return value_traits<V>::fingerprint(value); });
  }

  std::uint64_t checksum_all() const {
    std::uint64_t sum = 0;
    map_.scan([&](const K& key, const V& value) {
      sum += checksum::of(key, value);
      return true;
    });
    return sum;
  }

  std::uint64_t checksum_range(const K& start, std::size_t count) const {
    std::uint64_t sum = 0;
    std::size_t visited = 0;
    if (count > 0) {
      map_.scan_from(start, [&](const K& key, const V& value) {
        sum += checksum::of(key, value);
        ++visited;
        return visited != count;
      });
    }
    return sum;
  }

  bool remove(const K& key) {
    if (!map_.remove(key)) {
      return false;
    }
    --entries_;
    return true;
  }

  void bump_count(K) { unsupported_operation::reached(operation::entry_upsert); }

  std::optional<std::pair<K, V>> pop_first() { unsupported_operation::reached(operation::pop_first); }

  static counted_bbtree from_sorted_entries(entry_list<K, V>) {
    unsupported_operation::reached(operation::bulk_build);
  }

 private:
  static constexpr std::array lacking{operation::bulk_build, operation::entry_upsert, operation::pop_first};

  bbtree::bbtree_map<K, V> map_;
  std::size_t entries_ = 0;
};

}  // namespace bbtree_bench
