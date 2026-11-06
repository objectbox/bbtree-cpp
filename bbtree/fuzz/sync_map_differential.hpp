// The shared map's operations against std::map, on one thread (the latched
// layer over the same nodes), for integer and string keys.
#pragma once

#include "fuzz_input.hpp"
#include "map_differential.hpp"

#include <bbtree/map.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace bbtree_fuzz {

template <class K>
class sync_map_differential {
 public:
  static void run(std::span<const std::uint8_t> bytes) {
    fuzz_input in(bytes);
    bbtree::bbtree_map<K, std::uint64_t> map;
    std::map<K, std::uint64_t> model;
    for (int step = 0; step < 4096 && !in.done(); ++step) {
      const std::uint8_t op = in.byte() % 8;
      const K key = key_of<K>::make(in);
      switch (op) {
        case 0:
        case 1: {
          const std::uint64_t value = in.u64();
          const auto old = map.insert_or_assign(key, value);
          const auto expected = model.find(key);
          require(old.has_value() == (expected != model.end()), "insert_or_assign");
          model.insert_or_assign(key, value);
          break;
        }
        case 2:
          require(map.insert(key, op) == model.try_emplace(key, op).second, "insert");
          break;
        case 3:
        case 4:
          require(map.erase(key) == (model.erase(key) == 1), "erase");
          break;
        case 5: {
          const auto value = map.get_copied(key);
          const auto expected = model.find(key);
          require(value.has_value() == (expected != model.end()), "get_copied");
          if (value) {
            require(*value == expected->second, "get_copied value");
          }
          break;
        }
        case 6: {
          auto expected = model.lower_bound(key);
          std::size_t left = in.below(100);
          bool agreed = true;
          map.scan_from(key, [&](const K& stored, const std::uint64_t& value) {
            if (expected == model.end() || stored != expected->first || value != expected->second) {
              agreed = false;
              return false;
            }
            ++expected;
            return --left > 0;
          });
          require(agreed, "scan_from");
          break;
        }
        default:
          map.update(key, [](const K&, std::uint64_t& value) { ++value; });
          if (const auto at = model.find(key); at != model.end()) {
            ++at->second;
          }
      }
      map.check_invariants(model.size());
    }
  }

 private:
  static void require(bool holds, const char* what) {
    if (!holds) {
      throw invariant_failure(what);
    }
  }
};

}  // namespace bbtree_fuzz
