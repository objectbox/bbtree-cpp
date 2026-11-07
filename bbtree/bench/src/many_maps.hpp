// Operations on many tiny maps, all alive at once, as JSON objects are after
// parsing: the Rust bench's `operations/many_maps.rs`. The cost that matters
// is per map: its memory, its construction, and the pointer chase into it.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "keys.hpp"
#include "map_adapters.hpp"
#include "map_size.hpp"
#include "operation.hpp"
#include "parallel_setup.hpp"
#include "random.hpp"
#include "stopwatch.hpp"
#include "values.hpp"

namespace bbtree_bench {

// Vocabulary indices for each of many maps, one map after another.
class picks_per_map {
 public:
  picks_per_map() = default;

  static picks_per_map with_capacity(std::size_t map_count, std::size_t pick_count) {
    picks_per_map maps;
    maps.picks_.reserve(pick_count);
    maps.ends_.reserve(map_count);
    return maps;
  }

  void push_map(std::span<const std::uint16_t> picks) {
    picks_.insert(picks_.end(), picks.begin(), picks.end());
    ends_.push_back(picks_.size());
  }

  std::size_t map_count() const noexcept { return ends_.size(); }
  std::size_t pick_count() const noexcept { return picks_.size(); }

  // The picks of map `map`.
  std::span<const std::uint16_t> map_picks(std::size_t map) const noexcept {
    const std::size_t start = map == 0 ? 0 : ends_[map - 1];
    return std::span(picks_).subspan(start, ends_[map] - start);
  }

  // Every pick, one map after another.
  std::span<const std::uint16_t> all_picks() const noexcept { return picks_; }

  // How many times each index of a vocabulary of `vocabulary_size` keys is
  // picked.
  std::vector<std::size_t> pick_counts(std::size_t vocabulary_size) const {
    std::vector<std::size_t> counts(vocabulary_size, 0);
    for (const std::uint16_t pick : picks_) {
      ++counts[pick];
    }
    return counts;
  }

 private:
  std::vector<std::uint16_t> picks_;
  // Where each map's picks end in `picks_`.
  std::vector<std::size_t> ends_;
};

// The keys of many tiny maps, as indices into a vocabulary that they share,
// as JSON objects of one kind share field names.
template <class K>
class many_maps_keys {
 public:
  many_maps_keys(std::vector<K> vocabulary, picks_per_map maps)
      : vocabulary_(std::move(vocabulary)), maps_(std::move(maps)) {
    bench_error::unless(
        std::ranges::all_of(maps_.all_picks(), [&](std::uint16_t pick) { return pick < vocabulary_.size(); }),
        "every pick is in the vocabulary");
  }

  // `shape`'s maps, each of keys drawn at random from a vocabulary of
  // `map_shape::vocabulary_size` generated keys, in random order.
  static many_maps_keys generated(key_maker<K> keys, map_shape shape) {
    std::vector<K> vocabulary;
    for (std::uint64_t index = 0; index < map_shape::vocabulary_size; ++index) {
      vocabulary.push_back(keys.key(2 * index));
    }
    seeded_rng rng(random_stream::insertion_order);
    std::vector<std::uint16_t> indices(map_shape::vocabulary_size);
    std::iota(indices.begin(), indices.end(), std::uint16_t{0});
    const std::size_t entries = shape.map_count() * shape.entries_per_map();
    picks_per_map maps = picks_per_map::with_capacity(shape.map_count(), entries);
    for (std::size_t map = 0; map < shape.map_count(); ++map) {
      rng.choose_to_front(std::span(indices), shape.entries_per_map());
      maps.push_map(std::span(indices).first(shape.entries_per_map()));
    }
    return many_maps_keys(std::move(vocabulary), std::move(maps));
  }

  // `keys` as keys of this type, each made from its counterpart there.
  template <class From>
  static many_maps_keys converted_from(const many_maps_keys<From>& keys) {
    return many_maps_keys(std::vector<K>(keys.vocabulary().begin(), keys.vocabulary().end()), keys.maps());
  }

  std::size_t map_count() const noexcept { return maps_.map_count(); }
  std::size_t entry_count() const noexcept { return maps_.pick_count(); }
  const picks_per_map& maps() const noexcept { return maps_; }
  const std::vector<K>& vocabulary() const noexcept { return vocabulary_; }

  const K& key(std::uint16_t pick) const noexcept { return vocabulary_[pick]; }

  // For each map, as many vocabulary keys that it doesn't hold as it holds.
  // Each is the key of an entry drawn at random from all maps, drawn again
  // while this map holds it, so that misses probe the same mix of keys as
  // hits do.
  picks_per_map absent_picks() const {
    const std::vector<std::size_t> counts = maps_.pick_counts(vocabulary_.size());
    const auto occurring_keys =
        static_cast<std::size_t>(std::ranges::count_if(counts, [](std::size_t count) { return count > 0; }));
    seeded_rng rng(random_stream::absent_keys);
    picks_per_map absent;
    std::vector<std::uint16_t> map_absent;
    const std::span<const std::uint16_t> all_picks = maps_.all_picks();
    for (std::size_t map = 0; map < maps_.map_count(); ++map) {
      const std::span<const std::uint16_t> map_picks = maps_.map_picks(map);
      bench_error::unless(map_picks.size() < occurring_keys, "a map holds every key, so no key misses it");
      map_absent.clear();
      while (map_absent.size() < map_picks.size()) {
        const std::uint16_t pick = all_picks[rng.below(all_picks.size())];
        if (std::ranges::find(map_picks, pick) == map_picks.end()) {
          map_absent.push_back(pick);
        }
      }
      absent.push_map(map_absent);
    }
    return absent;
  }

 private:
  // Lookups probe with these, which stay in cache like the string literals
  // a program looks fields up with.
  std::vector<K> vocabulary_;
  // Each map's keys, distinct, in insertion order.
  picks_per_map maps_;
};

template <class K, class V>
class many_maps_cell {
 public:
  many_maps_cell(many_maps_keys<K> keys, value_maker<V> values) : keys_(std::move(keys)), values_(values) {}

  // Times `op` on maps of type `M`; returns how many operations it timed.
  template <class M>
  std::size_t time(operation op, stopwatch& watch) {
    switch (op) {
      case operation::build_many:
        return time_builds<M>(watch);
      case operation::get_many: {
        const auto [lookups, hits] = time_lookups<M>(keys_.maps(), watch);
        bench_error::unless(hits == lookups, "every get_many key is in its map");
        return lookups;
      }
      case operation::get_many_miss: {
        const picks_per_map absent = keys_.absent_picks();
        const auto [lookups, hits] = time_lookups<M>(absent, watch);
        bench_error::unless(hits == 0, "no get_many_miss key is in its map");
        return lookups;
      }
      case operation::iterate:
        return time_iteration<M>(watch);
      default:
        break;
    }
    throw bench_error(std::format("{} runs on one map", named<operation>::name(op)));
  }

  entry_list<K, V> build_input(operation op) const {
    bench_error::unless(op == operation::build_many, "only build_many builds many maps");
    return entries();
  }

  template <class M>
  std::vector<M> build(operation, entry_list<K, V> input) const {
    return build_maps<M>(std::move(input));
  }

  // Runs `visit(name, sequence)` on the vocabulary and on every order of
  // picks and maps the cell's operations use: for the self-check, which pins
  // them to the Rust bench's.
  template <class Visit>
  void for_each_order(Visit&& visit) const {
    const auto widened = [](std::span<const std::uint16_t> picks) {
      return std::vector<std::uint64_t>(picks.begin(), picks.end());
    };
    const auto ends = [](const picks_per_map& maps) {
      std::vector<std::uint64_t> sizes;
      for (std::size_t map = 0; map < maps.map_count(); ++map) {
        sizes.push_back(maps.map_picks(map).size());
      }
      return sizes;
    };
    visit("vocabulary", keys_.vocabulary());
    visit("picks", widened(keys_.maps().all_picks()));
    visit("map sizes", ends(keys_.maps()));
    const picks_per_map absent = keys_.absent_picks();
    visit("absent picks", widened(absent.all_picks()));
    const std::vector<std::size_t> order = map_visit_order(keys_.map_count());
    visit("map order", std::vector<std::uint64_t>(order.begin(), order.end()));
  }

 private:
  // The order lookups visit `map_count` maps in.
  static std::vector<std::size_t> map_visit_order(std::size_t map_count) {
    std::vector<std::size_t> map_order(map_count);
    std::iota(map_order.begin(), map_order.end(), std::size_t{0});
    seeded_rng(random_stream::map_order).shuffle(map_order);
    return map_order;
  }

  // Fresh entries for every map, one map after another.
  entry_list<K, V> entries() const {
    return parallel_setup::map(keys_.maps().all_picks(), [this](std::size_t index, std::uint16_t pick) {
      return std::pair<K, V>(keys_.key(pick), values_.value(index));
    });
  }

  // Builds each map from its share of `entries`, in their order.
  template <class M>
  std::vector<M> build_maps(entry_list<K, V> input) const {
    std::vector<M> maps;
    maps.reserve(keys_.map_count());
    std::size_t next = 0;
    for (std::size_t map = 0; map < keys_.map_count(); ++map) {
      M& built = maps.emplace_back();
      for (const std::size_t end = next + keys_.maps().map_picks(map).size(); next < end; ++next) {
        built.insert(std::move(input[next].first), std::move(input[next].second));
      }
    }
    return maps;
  }

  template <class M>
  std::size_t time_builds(stopwatch& watch) const {
    entry_list<K, V> input = entries();
    const std::vector<M> maps = watch.time([&] { return build_maps<M>(std::move(input)); });
    std::size_t entry_count = 0;
    for (const M& map : maps) {
      entry_count += map.entry_count();
    }
    bench_error::unless(entry_count == keys_.entry_count(), "the maps hold every entry");
    return entry_count;
  }

  // Looks up each map's `probes` in it, visiting the maps in random order.
  // Returns how many lookups it timed and how many of them hit.
  template <class M>
  std::pair<std::size_t, std::size_t> time_lookups(const picks_per_map& probes, stopwatch& watch) const {
    const std::vector<M> maps = build_maps<M>(entries());
    const std::vector<std::size_t> map_order = map_visit_order(maps.size());
    // Each visit's map and probes, found beforehand, so that the timed loop
    // reads only them.
    std::vector<std::pair<const M*, std::span<const std::uint16_t>>> visits;
    visits.reserve(map_order.size());
    for (const std::size_t map : map_order) {
      visits.emplace_back(&maps[map], probes.map_picks(map));
    }
    const auto [hits, checksum] = watch.time([&] {
      std::size_t hit_count = 0;
      std::uint64_t sum = 0;
      for (const auto& [map, map_probes] : visits) {
        for (const std::uint16_t pick : map_probes) {
          if constexpr (M::lends_references) {
            if (const V* value = map->get(keys_.key(pick))) {
              ++hit_count;
              sum += value_traits<V>::fingerprint(*value);
            }
          } else {
            if (const std::optional<std::uint64_t> fingerprint = map->lookup_fingerprint(keys_.key(pick))) {
              ++hit_count;
              sum += *fingerprint;
            }
          }
        }
      }
      return std::pair(hit_count, sum);
    });
    (void)checksum;
    return {probes.pick_count(), hits};
  }

  template <class M>
  std::size_t time_iteration(stopwatch& watch) const {
    const std::vector<M> maps = build_maps<M>(entries());
    watch.time([&] {
      std::uint64_t sum = 0;
      for (const M& map : maps) {
        sum += map.checksum_all();
      }
      return sum;
    });
    return keys_.entry_count();
  }

  many_maps_keys<K> keys_;
  value_maker<V> values_;
};

}  // namespace bbtree_bench
