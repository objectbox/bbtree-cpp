// Operations on one map: the Rust bench's `operations/single_map.rs`.
//
// A cell times at least the operation budget: operations that consume the
// map (builds, removals, pops) rebuild it for another round, outside the
// timed region, and lookups, scans and passes run more of themselves on the
// same map. Large maps run one round; lookups on them stop at the budget.
#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <utility>
#include <vector>

#include "keys.hpp"
#include "map_adapters.hpp"
#include "operation.hpp"
#include "parallel_setup.hpp"
#include "random.hpp"
#include "stopwatch.hpp"
#include "values.hpp"

namespace bbtree_bench {

// A one-map cell's keys: those the map holds, and those that lookups that
// miss probe with.
template <class K>
class single_map_keys {
 public:
  // `present` distinct; `absent` distinct, none of them in `present`, and
  // spread over its key range.
  single_map_keys(std::vector<K> present, std::vector<K> absent)
      : present_(std::move(present)), absent_(std::move(absent)) {}

  // `entries` generated keys at even indices, and keys at odd indices for
  // misses: as many as `miss_limit` or the entries, whichever is fewer,
  // spread over the range.
  static single_map_keys generated(key_maker<K> keys, std::size_t entries, std::size_t miss_limit) {
    std::vector<K> present;
    present.reserve(entries);
    for (std::uint64_t index = 0; index < entries; ++index) {
      present.push_back(keys.key(2 * index));
    }
    const std::size_t miss_count = std::min(miss_limit, entries);
    const std::size_t stride = entries / miss_count;
    std::vector<K> absent;
    absent.reserve(miss_count);
    for (std::size_t position = 0; position < miss_count; ++position) {
      absent.push_back(keys.key(2 * static_cast<std::uint64_t>(position * stride) + 1));
    }
    return single_map_keys(std::move(present), std::move(absent));
  }

  const std::vector<K>& present() const noexcept { return present_; }
  const std::vector<K>& absent() const noexcept { return absent_; }

  // The keys, for the cell that times them.
  std::pair<std::vector<K>, std::vector<K>> take() && { return {std::move(present_), std::move(absent_)}; }

 private:
  std::vector<K> present_;
  std::vector<K> absent_;
};

template <class K, class V>
class single_map_cell {
 public:
  single_map_cell(single_map_keys<K> keys, value_maker<V> values, std::size_t budget)
      : values_(values), budget_(budget) {
    std::tie(present_, absent_) = std::move(keys).take();
    seeded_rng(random_stream::insertion_order).shuffle(present_);
  }

  // Times `op` on maps of type `M`; returns how many operations it timed.
  template <class M>
  std::size_t time(operation op, stopwatch& watch) {
    switch (op) {
      case operation::random_insert:
      case operation::sequential_insert:
      case operation::bulk_build:
        return time_builds<M>(op, watch);
      case operation::get_hit: {
        const std::vector<K> pool = present_sample();
        const auto [lookups, hits] = time_lookups<M>(pool, watch);
        bench_error::unless(hits == lookups, "every get_hit key is in the map");
        return lookups;
      }
      case operation::get_miss: {
        const auto [lookups, hits] = time_lookups<M>(absent_, watch);
        bench_error::unless(hits == 0, "no get_miss key is in the map");
        return lookups;
      }
      case operation::iterate:
        return time_iteration<M>(watch);
      case operation::range64:
        return time_range_scans<M>(watch);
      case operation::random_remove:
        return time_removals<M>(watch);
      case operation::entry_upsert:
        return time_upserts<M>(watch);
      case operation::pop_first:
        return time_pops<M>(watch);
      case operation::build_many:
      case operation::get_many:
      case operation::get_many_miss:
        break;
    }
    throw bench_error(std::format("{} runs on many maps", named<operation>::name(op)));
  }

  // The entries that build `op` consumes, in its order.
  entry_list<K, V> build_input(operation op) const {
    switch (op) {
      case operation::random_insert:
        return entries_in_insertion_order();
      case operation::sequential_insert:
      case operation::bulk_build:
        return entries_in_key_order();
      default:
        throw bench_error(std::format("{} doesn't build a map", named<operation>::name(op)));
    }
  }

  // Builds the maps from `input` as build `op` does.
  template <class M>
  std::vector<M> build(operation op, entry_list<K, V> input) const {
    std::vector<M> maps;
    maps.push_back(build_map<M>(op, std::move(input)));
    return maps;
  }

  // Runs `visit(name, keys)` on every key order the cell's operations use,
  // each as its first round makes it: for the self-check, which pins them to
  // the Rust bench's.
  template <class Visit>
  void for_each_order(Visit&& visit) const {
    const auto probed_keys = [](const std::vector<const K*>& probe_order) {
      std::vector<K> keys;
      keys.reserve(probe_order.size());
      for (const K* key : probe_order) {
        keys.push_back(*key);
      }
      return keys;
    };
    visit("insertion order", present_);
    visit("absent", absent_);
    const std::vector<K> pool = present_sample();
    visit("hit probes", probed_keys(probes(pool)));
    visit("miss probes", probed_keys(probes(absent_)));
    const std::vector<K> starts = range_starts();
    visit("range probes", probed_keys(probes(starts)));
    seeded_rng removal_rng(random_stream::removal_order);
    visit("removal order", next_removal_order(removal_rng));
    seeded_rng upsert_rng(random_stream::insertion_order);
    visit("upsert order", next_upsert_order(upsert_rng));
  }

 private:
  std::size_t entry_count() const noexcept { return present_.size(); }

  // How many rounds reach the budget: one for large maps.
  std::size_t rounds() const noexcept { return (budget_ + entry_count() - 1) / entry_count(); }

  // Fresh entries, in insertion order.
  entry_list<K, V> entries_in_insertion_order() const {
    return parallel_setup::map(std::span<const K>(present_), [this](std::size_t index, const K& key) {
      return std::pair<K, V>(key, values_.value(index));
    });
  }

  entry_list<K, V> entries_in_key_order() const {
    entry_list<K, V> entries = entries_in_insertion_order();
    parallel_setup::sort(entries,
                         [](const auto& left, const auto& right) { return left.first < right.first; });
    return entries;
  }

  template <class M>
  static M insert_all(entry_list<K, V> entries) {
    M map;
    for (auto& [key, value] : entries) {
      map.insert(std::move(key), std::move(value));
    }
    return map;
  }

  // The map that operations other than builds start from: every key
  // inserted in random order, as most maps are built.
  template <class M>
  M built_map() const {
    return insert_all<M>(entries_in_insertion_order());
  }

  template <class M>
  static M build_map(operation op, entry_list<K, V> input) {
    if (op == operation::bulk_build) {
      return M::from_sorted_entries(std::move(input));
    }
    return insert_all<M>(std::move(input));
  }

  // Up to `budget` of the map's keys, a random sample, copied so that
  // lookups probe with keys of their own.
  std::vector<K> present_sample() const {
    const std::size_t count = std::min(budget_, entry_count());
    const std::size_t stride = entry_count() / count;
    std::vector<const K*> sample;
    sample.reserve(count);
    for (std::size_t index = 0; index < entry_count() && sample.size() < count; index += stride) {
      sample.push_back(&present_[index]);
    }
    return parallel_setup::map(std::span<const K* const>(sample),
                               [](std::size_t, const K* key) { return K(*key); });
  }

  // The keys range scans may start at: those with at least
  // `range_scan_length` - 1 keys after them, so that every scan visits the
  // full length.
  std::vector<K> range_starts() const {
    std::vector<K> starts = parallel_setup::cloned(std::span<const K>(present_));
    parallel_setup::sort(starts, std::less<K>());
    const std::size_t scan_length = enum_traits<operation>::range_scan_length;
    const std::size_t start_count = starts.size() >= scan_length - 1 ? starts.size() - (scan_length - 1) : 0;
    bench_error::unless(start_count > 0, "range64 needs at least 64 entries");
    starts.resize(start_count);
    return starts;
  }

  // The order lookups probe in: `budget` keys drawn at random from `pool`.
  // On small maps every key is probed many times, in an order too long for
  // the branch predictor to learn.
  std::vector<const K*> probes(const std::vector<K>& pool) const {
    seeded_rng rng(random_stream::lookup_order);
    std::vector<const K*> chosen;
    chosen.reserve(budget_);
    for (std::size_t probe = 0; probe < budget_; ++probe) {
      chosen.push_back(&pool[rng.below(pool.size())]);
    }
    return chosen;
  }

  template <class M>
  std::size_t time_builds(operation op, stopwatch& watch) const {
    for (std::size_t round = 0; round < rounds(); ++round) {
      entry_list<K, V> input = build_input(op);
      const M map = watch.time([&] { return build_map<M>(op, std::move(input)); });
      bench_error::unless(map.entry_count() == entry_count(), "the build holds every entry");
    }
    return rounds() * entry_count();
  }

  // Returns how many lookups it timed and how many of them hit.
  template <class M>
  std::pair<std::size_t, std::size_t> time_lookups(const std::vector<K>& pool, stopwatch& watch) const {
    const M map = built_map<M>();
    const std::vector<const K*> probe_order = probes(pool);
    // Maps that lend no references (`bbtree::bbtree_map`) read through a
    // closure; the others look up as they would in a program.
    const auto [hits, checksum] = watch.time([&] {
      std::size_t hit_count = 0;
      std::uint64_t sum = 0;
      for (const K* key : probe_order) {
        if constexpr (M::lends_references) {
          if (const V* value = map.get(*key)) {
            ++hit_count;
            sum += value_traits<V>::fingerprint(*value);
          }
        } else {
          if (const std::optional<std::uint64_t> fingerprint = map.lookup_fingerprint(*key)) {
            ++hit_count;
            sum += *fingerprint;
          }
        }
      }
      return std::pair(hit_count, sum);
    });
    (void)checksum;
    return {probe_order.size(), hits};
  }

  template <class M>
  std::size_t time_iteration(stopwatch& watch) const {
    const M map = built_map<M>();
    const std::size_t passes = rounds();
    // The opaque pointer keeps the passes from being merged into one.
    watch.time([&] {
      std::uint64_t sum = 0;
      for (std::size_t pass = 0; pass < passes; ++pass) {
        sum += optimizer_barrier::opaque(&map)->checksum_all();
      }
      return sum;
    });
    return passes * entry_count();
  }

  template <class M>
  std::size_t time_range_scans(stopwatch& watch) const {
    const M map = built_map<M>();
    const std::vector<K> starts = range_starts();
    const std::vector<const K*> probe_order = probes(starts);
    watch.time([&] {
      std::uint64_t sum = 0;
      for (const K* start : probe_order) {
        sum += map.checksum_range(*start, enum_traits<operation>::range_scan_length);
      }
      return sum;
    });
    return probe_order.size();
  }

  // The next round's order of removals: every key, shuffled.
  std::vector<K> next_removal_order(seeded_rng& rng) const {
    std::vector<K> removal_order = parallel_setup::cloned(std::span<const K>(present_));
    rng.shuffle(removal_order);
    return removal_order;
  }

  // The next round's order of upserts: every key twice, shuffled.
  std::vector<K> next_upsert_order(seeded_rng& rng) const {
    std::vector<K> upserted_keys = parallel_setup::cloned(std::span<const K>(present_));
    std::vector<K> second_copy = parallel_setup::cloned(std::span<const K>(present_));
    upserted_keys.insert(upserted_keys.end(), std::make_move_iterator(second_copy.begin()),
                         std::make_move_iterator(second_copy.end()));
    rng.shuffle(upserted_keys);
    return upserted_keys;
  }

  template <class M>
  std::size_t time_removals(stopwatch& watch) const {
    seeded_rng rng(random_stream::removal_order);
    for (std::size_t round = 0; round < rounds(); ++round) {
      M map = built_map<M>();
      const std::vector<K> removal_order = next_removal_order(rng);
      const std::size_t removed = watch.time([&] {
        std::size_t count = 0;
        for (const K& key : removal_order) {
          if (map.remove(key)) {
            ++count;
          }
        }
        return count;
      });
      bench_error::unless(removed == entry_count(), "every key is removed");
    }
    return rounds() * entry_count();
  }

  template <class M>
  std::size_t time_upserts(stopwatch& watch) const {
    seeded_rng rng(random_stream::insertion_order);
    for (std::size_t round = 0; round < rounds(); ++round) {
      std::vector<K> upserted_keys = next_upsert_order(rng);
      const M map = watch.time([&] {
        M upserted;
        for (K& key : upserted_keys) {
          upserted.bump_count(std::move(key));
        }
        return upserted;
      });
      bench_error::unless(map.entry_count() == entry_count(), "every key is upserted");
    }
    return rounds() * 2 * entry_count();
  }

  template <class M>
  std::size_t time_pops(stopwatch& watch) const {
    for (std::size_t round = 0; round < rounds(); ++round) {
      M map = built_map<M>();
      const auto [popped, checksum] = watch.time([&] {
        std::size_t count = 0;
        std::uint64_t sum = 0;
        while (std::optional<std::pair<K, V>> entry = map.pop_first()) {
          ++count;
          sum += key_traits<K>::fingerprint(entry->first) ^ value_traits<V>::fingerprint(entry->second);
        }
        return std::pair(count, sum);
      });
      (void)checksum;
      bench_error::unless(popped == entry_count(), "every entry is popped");
    }
    return rounds() * entry_count();
  }

  // The map's keys, distinct, in random insertion order.
  std::vector<K> present_;
  // Keys that the map doesn't hold, which lookups that miss probe with.
  std::vector<K> absent_;
  value_maker<V> values_;
  std::size_t budget_;
};

}  // namespace bbtree_bench
