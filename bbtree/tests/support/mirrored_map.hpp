// A map of tracked keys and values (`instrumented.hpp`) beside a model of
// which objects it must hold: every operation goes to both, and the map must
// then hold, by object id, the very key and value objects the model names
// (the Rust tests' mirrored maps, `differential.rs` and `drops.rs`). A map
// that copied where it should move, kept the new key where the old one stays,
// or handed back another object than the stored one differs from its model.
#pragma once

#include "checked_pages.hpp"
#include "instrumented.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

namespace bbtree_test {

// The objects of one entry.
struct entry_ids {
  object_ledger::object_id key;
  object_ledger::object_id value;
  friend bool operator==(const entry_ids&, const entry_ids&) = default;
};

// What a map of tracked keys and values holds, in order: each entry's order
// and objects. Two snapshots are equal when nothing was added, removed,
// replaced or copied in between.
template <class Map>
struct held_entries {
  using entries = std::vector<std::pair<std::uint64_t, entry_ids>>;

  static entries forward(const Map& map) {
    entries held;
    for (auto at = map.begin(); at != map.end(); ++at) {
      held.push_back({at->first.order(), {at->first.id(), at->second.id()}});
    }
    return held;
  }
  static entries backward(const Map& map) {
    entries held;
    for (auto at = map.end(); at != map.begin();) {
      --at;
      held.push_back({at->first.order(), {at->first.id(), at->second.id()}});
    }
    std::reverse(held.begin(), held.end());
    return held;
  }
  static entries by_for_each(const Map& map) {
    entries held;
    map.for_each([&](const typename Map::key_type& key, const typename Map::mapped_type& value) {
      held.push_back({key.order(), {key.id(), value.id()}});
    });
    return held;
  }
};

template <class Map>
class mirrored_map {
  using K = typename Map::key_type;
  using V = typename Map::mapped_type;
  static constexpr bool copies_entries = std::is_copy_constructible_v<K> && std::is_copy_constructible_v<V>;

 public:
  using model_type = std::map<std::uint64_t, entry_ids>;

  mirrored_map(object_ledger& ledger, page_book& pages) : ledger_(&ledger), map_(checked_allocator(pages)) {}

  Map& map() noexcept { return map_; }
  const Map& map() const noexcept { return map_; }
  model_type& model() noexcept { return model_; }
  const model_type& model() const noexcept { return model_; }

  K key(std::uint64_t order) const { return K(*ledger_, order); }
  V value(std::uint64_t payload) const { return V(*ledger_, payload); }

  // The map is well formed and holds the model's objects, walked forward,
  // backward and by `for_each`.
  void require_same() const {
    map_.check_invariants();
    const typename held_entries<Map>::entries expected(model_.begin(), model_.end());
    REQUIRE(map_.size() == expected.size());
    REQUIRE(map_.empty() == expected.empty());
    REQUIRE(held_entries<Map>::forward(map_) == expected);
    REQUIRE(held_entries<Map>::backward(map_) == expected);
    REQUIRE(held_entries<Map>::by_for_each(map_) == expected);
  }

  // `at` is at the model's entry of `order`.
  void require_at(typename Map::const_iterator at, std::uint64_t order) const {
    REQUIRE(at != map_.end());
    REQUIRE(at->first.order() == order);
    REQUIRE(entry_ids{at->first.id(), at->second.id()} == model_.at(order));
  }

  // --- Inserting ---

  // `try_emplace` leaves its arguments alone when the key is there.
  void try_emplace_moved(std::uint64_t order, std::uint64_t payload) {
    K new_key = key(order);
    V new_value = value(payload);
    const entry_ids fresh{new_key.id(), new_value.id()};
    const auto [at, inserted] = map_.try_emplace(std::move(new_key), std::move(new_value));
    REQUIRE(inserted == !model_.contains(order));
    REQUIRE(new_key.moved_from() == inserted);
    REQUIRE(new_value.moved_from() == inserted);
    if (inserted) {
      model_.emplace(order, fresh);
    }
    require_at(at, order);
  }

  void try_emplace_copied(std::uint64_t order, std::uint64_t payload)
    requires copies_entries
  {
    const K probe = key(order);
    const auto [at, inserted] = map_.try_emplace(probe, *ledger_, payload);
    REQUIRE(inserted == !model_.contains(order));
    if (inserted) {
      REQUIRE(at->first.id() != probe.id());
      model_.emplace(order, entry_ids{at->first.id(), at->second.id()});
    }
    require_at(at, order);
  }

  // The stored key stays and the value is replaced (the Rust crate's
  // `insert`).
  void insert_or_assign_moved(std::uint64_t order, std::uint64_t payload) {
    K new_key = key(order);
    V new_value = value(payload);
    const entry_ids fresh{new_key.id(), new_value.id()};
    const auto [at, inserted] = map_.insert_or_assign(std::move(new_key), std::move(new_value));
    REQUIRE(inserted == !model_.contains(order));
    REQUIRE(new_key.moved_from() == inserted);
    REQUIRE(new_value.moved_from());
    if (inserted) {
      model_.emplace(order, fresh);
    } else {
      model_.at(order).value = fresh.value;
    }
    require_at(at, order);
  }

  void emplace_pair(std::uint64_t order, std::uint64_t payload) {
    const auto [at, inserted] = map_.emplace(key(order), value(payload));
    REQUIRE(inserted == !model_.contains(order));
    if (inserted) {
      model_.emplace(order, entry_ids{at->first.id(), at->second.id()});
    }
    require_at(at, order);
  }

  void insert_value_type(std::uint64_t order, std::uint64_t payload)
    requires copies_entries
  {
    const typename Map::value_type entry{key(order), value(payload)};
    const auto [at, inserted] = map_.insert(entry);
    REQUIRE(inserted == !model_.contains(order));
    if (inserted) {
      REQUIRE(at->second.payload() == payload);
      model_.emplace(order, entry_ids{at->first.id(), at->second.id()});
    }
    require_at(at, order);
  }

  void assign_through_iterator(std::uint64_t order, std::uint64_t payload) {
    const auto at = map_.find(key(order));
    REQUIRE((at == map_.end()) == !model_.contains(order));
    if (at != map_.end()) {
      at->second = value(payload);
      model_.at(order).value = at->second.id();
    }
  }

  // --- Looking up ---

  void look_up(std::uint64_t order) const {
    const K probe = key(order);
    const auto expected = model_.find(order);
    const bool held = expected != model_.end();
    const V* found = map_.get(probe);
    REQUIRE((found != nullptr) == held);
    REQUIRE(map_.contains(probe) == held);
    REQUIRE(map_.count(probe) == (held ? 1u : 0u));
    if (held) {
      REQUIRE(found->id() == expected->second.value);
      REQUIRE(map_.at(probe).id() == expected->second.value);
      require_at(map_.find(probe), order);
    }
    require_bound(map_.lower_bound(probe), model_.lower_bound(order));
    require_bound(map_.upper_bound(probe), model_.upper_bound(order));
  }

  // --- Removing ---

  void erase_key(std::uint64_t order) { REQUIRE(map_.erase(key(order)) == model_.erase(order)); }

  // Erases through an iterator a lookup made; the returned one is at the
  // next entry.
  void erase_at(std::uint64_t order) {
    const auto at = map_.find(key(order));
    REQUIRE((at == map_.end()) == !model_.contains(order));
    if (at != map_.end()) {
      require_bound(map_.erase(at), model_.erase(model_.find(order)));
    }
  }

  // The stored key and value come back (the Rust crate's `remove_entry`).
  void remove_entry(std::uint64_t order) {
    const std::optional<std::pair<K, V>> removed = map_.remove(key(order));
    const auto expected = model_.find(order);
    REQUIRE(removed.has_value() == (expected != model_.end()));
    if (removed) {
      REQUIRE(entry_ids{removed->first.id(), removed->second.id()} == expected->second);
      model_.erase(expected);
    }
  }

  void pop_first() { require_popped(map_.pop_first(), model_.begin()); }
  void pop_last() {
    require_popped(map_.pop_last(), model_.empty() ? model_.end() : std::prev(model_.end()));
  }

  // Inserts the orders below `count` in a random order, each with the
  // payload `order + payload_base`.
  void fill_shuffled(std::uint64_t count, std::mt19937_64& rng, std::uint64_t payload_base = 0) {
    std::vector<std::uint64_t> orders(count);
    for (std::uint64_t order = 0; order < count; ++order) {
      orders[order] = order;
    }
    std::shuffle(orders.begin(), orders.end(), rng);
    for (const std::uint64_t order : orders) {
      try_emplace_moved(order, order + payload_base);
    }
  }

 private:
  void require_bound(typename Map::const_iterator at, model_type::const_iterator expected) const {
    REQUIRE((at == map_.end()) == (expected == model_.end()));
    if (expected != model_.end()) {
      require_at(at, expected->first);
    }
  }

  void require_popped(const std::optional<std::pair<K, V>>& popped, model_type::iterator expected) {
    REQUIRE(popped.has_value() == (expected != model_.end()));
    if (popped) {
      REQUIRE(entry_ids{popped->first.id(), popped->second.id()} == expected->second);
      model_.erase(expected);
    }
    REQUIRE(map_.first().has_value() == !model_.empty());
    if (!model_.empty()) {
      REQUIRE(map_.first()->first.id() == model_.begin()->second.key);
      REQUIRE(map_.last()->second.id() == model_.rbegin()->second.value);
    }
  }

  object_ledger* ledger_;
  Map map_;
  model_type model_;
};

}  // namespace bbtree_test
