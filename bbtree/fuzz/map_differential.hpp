// Map operations against std::map, one target per row of the node plan
// (D5, D47), which is where key types take different paths through the shared
// node code: integers, heap byte strings, byte arrays, keys with their own
// inline keys, and keys without any. Every operation is checked against
// std::map, and the tree's invariants after it (`BBTREE_CHECKED`).
#pragma once

#include "fuzz_input.hpp"

#include <bbtree/unsync/map.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace bbtree_fuzz {

// A key with its own inline keys, hash tags and comparisons past the bytes
// known equal (the plan's trait row).
struct user_key {
  std::string text;
  friend auto operator<=>(const user_key&, const user_key&) = default;
  friend bool operator==(const user_key&, const user_key&) = default;
  std::span<const std::uint8_t> bytes() const noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
  }
};

// Keys of each target from the input.
template <class K>
struct key_of;

template <>
struct key_of<std::uint64_t> {
  static std::uint64_t make(fuzz_input& in) {
    const std::uint64_t small = in.below(4096);
    return in.byte() % 4 == 0 ? in.u64() : small;
  }
};
template <>
struct key_of<std::string> {
  static std::string make(fuzz_input& in) { return in.text(40); }
};
template <>
struct key_of<std::array<std::uint8_t, 16>> {
  static std::array<std::uint8_t, 16> make(fuzz_input& in) {
    std::array<std::uint8_t, 16> key{};
    for (std::size_t i = 14; i < 16; ++i) {
      key[i] = in.byte();
    }
    key[0] = static_cast<std::uint8_t>(in.below(3));
    return key;
  }
};
template <>
struct key_of<user_key> {
  static user_key make(fuzz_input& in) { return {in.text(40)}; }
};

class invariant_failure : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

template <class K, class Strategy = bbtree::auto_inline_keys>
class map_differential {
  using map_type =
      bbtree::unsync::bbtree_map<K, std::uint64_t, std::less<K>, bbtree::global_allocator, Strategy>;
  using model_type = std::map<K, std::uint64_t>;

 public:
  // Runs the operations `bytes` describe; throws `invariant_failure` where
  // the map and std::map disagree.
  static void run(std::span<const std::uint8_t> bytes) {
    fuzz_input in(bytes);
    map_type map;
    model_type model;
    for (int step = 0; step < 4096 && !in.done(); ++step) {
      apply(in, map, model);
      map.check_invariants();
      require(map.size() == model.size(), "sizes differ");
    }
    require(std::equal(map.begin(), map.end(), model.begin(), model.end(),
                       [](auto a, const auto& b) { return a.first == b.first && a.second == b.second; }),
            "entries differ");
  }

 private:
  static void require(bool holds, const char* what) {
    if (!holds) {
      throw invariant_failure(what);
    }
  }

  static void apply(fuzz_input& in, map_type& map, model_type& model) {
    const std::uint8_t op = in.byte() % 16;
    const K key = key_of<K>::make(in);
    switch (op) {
      case 0:
      case 1:
      case 2: {
        const std::uint64_t value = in.u64();
        require(map.insert_or_assign(key, value).second == model.insert_or_assign(key, value).second,
                "insert_or_assign");
        break;
      }
      case 3:
        require(map.try_emplace(key, op).second == model.try_emplace(key, op).second, "try_emplace");
        break;
      case 4:
      case 5:
        require(map.erase(key) == model.erase(key), "erase");
        break;
      case 6: {
        const auto found = map.find(key);
        const auto expected = model.find(key);
        require((found == map.end()) == (expected == model.end()), "find");
        if (expected != model.end()) {
          require(found->second == expected->second, "found value");
        }
        break;
      }
      case 7: {
        // Bounds, then a walk from them, forward and back.
        auto at = map.lower_bound(key);
        auto expected = model.lower_bound(key);
        for (std::size_t steps = in.below(80); steps > 0; --steps) {
          require((at == map.end()) == (expected == model.end()), "lower_bound walk");
          if (expected == model.end()) {
            break;
          }
          require(at->first == expected->first, "walked key");
          ++at;
          ++expected;
        }
        auto upper = map.upper_bound(key);
        auto model_upper = model.upper_bound(key);
        if (upper != map.begin() && model_upper != model.begin()) {
          --upper;
          --model_upper;
          require(upper->first == model_upper->first, "upper_bound, back");
        }
        break;
      }
      case 8: {
        // Erase through iterators from a key on.
        auto at = map.lower_bound(key);
        auto expected = model.lower_bound(key);
        for (std::size_t steps = in.below(20); steps > 0 && at != map.end(); --steps) {
          require(at->first == expected->first, "erase walk");
          if (in.byte() % 2 == 0) {
            at = map.erase(at);
            expected = model.erase(expected);
          } else {
            ++at;
            ++expected;
          }
        }
        break;
      }
      case 9: {
        const auto popped = in.byte() % 2 == 0 ? map.pop_first() : map.pop_last();
        require(popped.has_value() == !model.empty(), "pop");
        if (popped) {
          const auto expected = model.find(popped->first);
          require(
              expected != model.end() && (expected == model.begin() || std::next(expected) == model.end()),
              "popped an end");
          model.erase(expected);
        }
        break;
      }
      case 10: {
        const std::uint64_t modulus = in.below(7) + 2;
        erase_if(map, [&](auto entry) { return entry.second % modulus == 0; });
        std::erase_if(model, [&](const auto& entry) { return entry.second % modulus == 0; });
        break;
      }
      case 11: {
        map_type upper = map.split_off(key);
        model_type model_upper(model.lower_bound(key), model.end());
        model.erase(model.lower_bound(key), model.end());
        upper.check_invariants();
        require(upper.size() == model_upper.size(), "split_off");
        if (in.byte() % 2 == 0) {
          map.append(upper);
          model.insert(model_upper.begin(), model_upper.end());
        }
        break;
      }
      case 12: {
        map_type copy = map;
        copy.check_invariants();
        require(copy == map, "copy");
        break;
      }
      case 13: {
        std::vector<std::pair<K, std::uint64_t>> entries;
        for (std::size_t count = in.below(200); count > 0; --count) {
          entries.emplace_back(key_of<K>::make(in), in.below(1000));
        }
        map_type built(entries.begin(), entries.end());
        built.check_invariants();
        require(built.size() == model_type(entries.begin(), entries.end()).size(), "bulk build");
        break;
      }
      default: {
        const std::uint64_t value = in.below(1000);
        map[key] += value;
        model[key] += value;
      }
    }
  }
};

}  // namespace bbtree_fuzz

template <>
struct bbtree::inline_key<bbtree_fuzz::user_key> {
  static bbtree::inline_word at(const bbtree_fuzz::user_key& key, std::size_t offset) noexcept {
    return bbtree::detail::big_endian::window(key.bytes(), offset);
  }
  static std::uint8_t hash_tag(const bbtree_fuzz::user_key& key) noexcept {
    return key.text.size() < 255 ? static_cast<std::uint8_t>(key.text.size()) : std::uint8_t{255};
  }
  static std::strong_ordering cmp_from(const bbtree_fuzz::user_key& a, const bbtree_fuzz::user_key& b,
                                       std::size_t equal_len) noexcept {
    return bbtree::detail::search_bytes(a.bytes()).cmp_from(b.bytes(), equal_len);
  }
};
