// Maps and sets of keys and values that can't be copied, through every member
// that takes an entry out or moves it: the Rust crate's test types are all
// move-only (`Tracked` is not `Clone`), so each of its tests is one of these.
#include <bbtree/unsync/map.hpp>
#include <bbtree/unsync/set.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <utility>

namespace {

// A key that owns its number and can only move.
class owned_number {
 public:
  explicit owned_number(std::uint64_t number) : number_(std::make_unique<std::uint64_t>(number)) {}

  std::uint64_t number() const noexcept { return *number_; }
  friend bool operator<(const owned_number& a, const owned_number& b) noexcept {
    return a.number() < b.number();
  }

 private:
  std::unique_ptr<std::uint64_t> number_;
};

// How a test names a key of type `K` by its number.
template <class K>
struct key_of_number {
  static K make(std::uint64_t number) { return K(number); }
  static std::uint64_t number(const K& key) { return static_cast<std::uint64_t>(key); }
};
template <>
struct key_of_number<owned_number> {
  static owned_number make(std::uint64_t number) { return owned_number(number); }
  static std::uint64_t number(const owned_number& key) { return key.number(); }
};

template <class K>
using owning_map = bbtree::unsync::bbtree_map<K, std::unique_ptr<std::uint64_t>>;

template <class K>
void require_same(const owning_map<K>& map, const std::map<std::uint64_t, std::uint64_t>& model) {
  map.check_invariants();
  REQUIRE(map.size() == model.size());
  auto expected = model.begin();
  for (const auto& [key, value] : map) {
    REQUIRE(key_of_number<K>::number(key) == expected->first);
    REQUIRE(*value == expected->second);
    ++expected;
  }
}

// Random changes through every member that removes or moves an entry,
// against a std::map of the numbers.
template <class K>
void run_changes_against_std_map() {
  using keys = key_of_number<K>;
  owning_map<K> map;
  std::map<std::uint64_t, std::uint64_t> model;
  std::mt19937_64 rng(3);
  for (int step = 0; step < 30'000; ++step) {
    const std::uint64_t number = rng() % 3000;
    const std::uint64_t payload = rng();
    switch (rng() % 12) {
      case 0:
      case 1:
      case 2:
        REQUIRE(map.try_emplace(keys::make(number), std::make_unique<std::uint64_t>(payload)).second ==
                model.try_emplace(number, payload).second);
        break;
      case 3:
        REQUIRE(map.insert_or_assign(keys::make(number), std::make_unique<std::uint64_t>(payload)).second ==
                model.insert_or_assign(number, payload).second);
        break;
      case 4:
        map[keys::make(number)] = std::make_unique<std::uint64_t>(payload);
        model[number] = payload;
        break;
      case 5:
        REQUIRE(map.emplace(keys::make(number), std::make_unique<std::uint64_t>(payload)).second ==
                model.emplace(number, payload).second);
        break;
      case 6:
        REQUIRE(map.erase(keys::make(number)) == model.erase(number));
        break;
      case 7: {
        const auto removed = map.remove(keys::make(number));
        REQUIRE(removed.has_value() == model.contains(number));
        if (removed) {
          REQUIRE(*removed->second == model.at(number));
          model.erase(number);
        }
        break;
      }
      case 8: {
        const auto at = map.find(keys::make(number));
        REQUIRE((at == map.end()) == !model.contains(number));
        if (at != map.end()) {
          const auto next = map.erase(at);
          const auto model_next = model.erase(model.find(number));
          REQUIRE((next == map.end()) == (model_next == model.end()));
        }
        break;
      }
      case 9: {
        const bool last = rng() % 2 == 0;
        const auto popped = last ? map.pop_last() : map.pop_first();
        REQUIRE(popped.has_value() == !model.empty());
        if (popped) {
          const auto expected = last ? std::prev(model.end()) : model.begin();
          REQUIRE(keys::number(popped->first) == expected->first);
          REQUIRE(*popped->second == expected->second);
          model.erase(expected);
        }
        break;
      }
      case 10:
        if (step % 16 == 0) {
          owning_map<K> upper = map.split_off(keys::make(number));
          REQUIRE(upper.size() ==
                  static_cast<std::size_t>(std::distance(model.lower_bound(number), model.end())));
          owning_map<K> moved = std::move(upper);
          map.append(moved);
        }
        break;
      default:
        if (step % 64 == 0) {
          const std::uint64_t residue = number % 7;
          const auto keep = [&](const K& key, std::unique_ptr<std::uint64_t>&) {
            return keys::number(key) % 7 != residue;
          };
          const std::size_t before = model.size();
          std::erase_if(model, [&](const auto& entry) { return entry.first % 7 == residue; });
          if (step % 128 == 0) {
            REQUIRE(map.retain(keep) == before - model.size());
          } else {
            REQUIRE(erase_if(map, [&](auto entry) { return !keep(entry.first, entry.second); }) ==
                    before - model.size());
          }
        }
        break;
    }
    if (step % 1024 == 0) {
      require_same(map, model);
    }
  }
  require_same(map, model);
  while (!model.empty()) {
    REQUIRE(map.erase(keys::make(model.begin()->first)) == 1);
    model.erase(model.begin());
  }
  require_same(map, model);
}

}  // namespace

TEST_CASE("a map of values that can only move erases, pops, retains and splits", "[map][move-only]") {
  run_changes_against_std_map<std::uint64_t>();
}

TEST_CASE("a map of keys that can only move erases, pops, retains and splits", "[map][move-only]") {
  run_changes_against_std_map<owned_number>();
}

TEST_CASE("a set of keys that can only move erases, takes and pops", "[set][move-only]") {
  bbtree::unsync::bbtree_set<owned_number> set;
  std::set<std::uint64_t> model;
  std::mt19937_64 rng(4);
  for (int step = 0; step < 20'000; ++step) {
    const std::uint64_t number = rng() % 3000;
    switch (rng() % 6) {
      case 0:
      case 1:
        REQUIRE(set.insert(owned_number(number)).second == model.insert(number).second);
        break;
      case 2:
        REQUIRE(set.emplace(number).second == model.insert(number).second);
        break;
      case 3:
        REQUIRE(set.erase(owned_number(number)) == model.erase(number));
        break;
      case 4: {
        const std::optional<owned_number> taken = set.take(owned_number(number));
        REQUIRE(taken.has_value() == (model.erase(number) == 1));
        if (taken) {
          REQUIRE(taken->number() == number);
        }
        break;
      }
      default: {
        const bool last = rng() % 2 == 0;
        const std::optional<owned_number> popped = last ? set.pop_last() : set.pop_first();
        REQUIRE(popped.has_value() == !model.empty());
        if (popped) {
          const auto expected = last ? std::prev(model.end()) : model.begin();
          REQUIRE(popped->number() == *expected);
          model.erase(expected);
        }
        break;
      }
    }
  }
  set.check_invariants();
  REQUIRE(set.size() == model.size());
  REQUIRE(set.retain([](const owned_number& key) { return key.number() % 2 == 0; }) ==
          std::erase_if(model, [](std::uint64_t number) { return number % 2 != 0; }));
  auto expected = model.begin();
  for (const owned_number& key : set) {
    REQUIRE(key.number() == *expected);
    ++expected;
  }
}
