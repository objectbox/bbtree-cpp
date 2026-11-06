// The operations on two sets against std's sets and algorithms: the only set
// code that is not the map's.
#pragma once

#include "fuzz_input.hpp"
#include "map_differential.hpp"

#include <bbtree/unsync/set.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <vector>

namespace bbtree_fuzz {

class set_algebra {
  using set_type = bbtree::unsync::bbtree_set<std::uint64_t>;

 public:
  static void run(std::span<const std::uint8_t> bytes) {
    fuzz_input in(bytes);
    set_type a;
    set_type b;
    std::set<std::uint64_t> model_a;
    std::set<std::uint64_t> model_b;
    for (std::size_t count = in.below(3000); count > 0; --count) {
      const std::uint64_t key = in.below(5000);
      a.insert(key);
      model_a.insert(key);
    }
    for (std::size_t count = in.below(3000); count > 0; --count) {
      const std::uint64_t key = in.below(5000);
      b.insert(key);
      model_b.insert(key);
    }
    check(a | b, model_a, model_b, [](auto... args) { return std::set_union(args...); });
    check(a & b, model_a, model_b, [](auto... args) { return std::set_intersection(args...); });
    check(a - b, model_a, model_b, [](auto... args) { return std::set_difference(args...); });
    check(a ^ b, model_a, model_b, [](auto... args) { return std::set_symmetric_difference(args...); });
    require(a.is_subset(b) == std::includes(model_b.begin(), model_b.end(), model_a.begin(), model_a.end()),
            "subset");
    require(a.is_disjoint(b) == (a & b).empty(), "disjoint");
  }

 private:
  static void require(bool holds, const char* what) {
    if (!holds) {
      throw invariant_failure(what);
    }
  }

  template <class Algorithm>
  static void check(const set_type& result, const std::set<std::uint64_t>& a,
                    const std::set<std::uint64_t>& b, Algorithm algorithm) {
    std::vector<std::uint64_t> expected;
    algorithm(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(expected));
    result.check_invariants();
    require(std::equal(result.begin(), result.end(), expected.begin(), expected.end()), "set algebra");
  }
};

}  // namespace bbtree_fuzz
