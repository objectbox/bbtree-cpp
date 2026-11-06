// The suites CI runs: lists of workloads, each of which runs on every map of
// its suite: the Rust bench's `suites.rs`, workload for workload.
#pragma once

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include "cell_filter.hpp"
#include "named.hpp"
#include "workload.hpp"

namespace bbtree_bench {

// Named collections of workloads: `small`, and `realkeys` and `tiny`
// without their datasets, to keep the bench building and running; `ci` to
// report each change's timings. Selected to keep each run below five minutes
// on the Rust bench's benchmark machine (Ryzen 9 9900X).
enum class suite {
  // Each large real-world key set -> `u64`, at 1M and 10M (`all` for sets of
  // fewer keys), on the ordered maps: inserts and lookups.
  real_keys,
  // Many tiny JSON-object-like maps: JSON keys -> 32-byte structs, in maps
  // of generated keys and in the objects of real JSON documents.
  tiny,
  // `u64 -> u64` and `string -> u64` at 1k, every operation; and crate names
  // and kernel paths -> `u64` at 1k: inserts and lookups.
  small,
  // What the Rust CI measures for every pull request, bbtree's unsync map
  // against the std B-tree only: `u64 -> u64` at 1k and at 1M every
  // operation, and at 10M inserts and lookups; and strings -> `u64` at 1k and
  // 1M, inserts and lookups. Generated keys only.
  ci,
};

template <>
struct enum_traits<suite> {
  static constexpr std::array all{suite::real_keys, suite::tiny, suite::small, suite::ci};

  static constexpr std::string_view name(suite s) noexcept {
    switch (s) {
      case suite::real_keys:
        return "realkeys";
      case suite::tiny:
        return "tiny";
      case suite::small:
        return "small";
      case suite::ci:
        return "ci";
    }
    return "";
  }

  // The maps the suite's workloads run on. The real-keys suite compares the
  // ordered maps only: at 10M entries of long strings, building each map
  // takes seconds, and the hash maps wouldn't fit its time budget. CI
  // compares bbtree's unsync map with its baseline, the std B-tree.
  static std::span<const map_kind> maps(suite s) noexcept;

  static std::vector<workload> workloads(suite s);
};

// A row of the report: a workload, under the first suite that lists it.
class suite_row {
 public:
  // The workloads of `suites` that `filter` admits, in suite order. A
  // workload that several suites list runs once.
  static std::vector<suite_row> select(std::span<const suite> suites, const cell_filter& filter);

  suite of_suite() const noexcept { return suite_; }
  const workload& work() const noexcept { return workload_; }

 private:
  suite_row(suite s, workload work) noexcept : suite_(s), workload_(work) {}

  suite suite_;
  workload workload_;
};

}  // namespace bbtree_bench
