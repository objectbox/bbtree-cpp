#include "suites.hpp"

#include <algorithm>

namespace bbtree_bench {

namespace {

// The key kinds read from large datasets of one key per line: those of the
// string-index literature (LITS, HOPE, HOT), but Wikipedia's titles of today.
constexpr std::array real_key_kinds{key_kind::wiki_titles, key_kind::urls, key_kind::addresses,
                                    key_kind::geo_names, key_kind::dblp_titles};

constexpr std::array inserts_hits_and_misses{operation::random_insert, operation::get_hit,
                                             operation::get_miss};

}  // namespace

std::span<const map_kind> enum_traits<suite>::maps(suite s) noexcept {
  static constexpr std::array ordered_baseline{map_kind::unsync, map_kind::std_btree};
  switch (s) {
    case suite::real_keys:
    case suite::ci:
      return ordered_baseline;
    case suite::tiny:
    case suite::small:
    case suite::cpp_maps:
      return enum_traits<map_kind>::all;
  }
  return {};
}

std::vector<workload> enum_traits<suite>::workloads(suite s) {
  constexpr std::array u64_values{value_kind::u64};
  constexpr std::array struct_values{value_kind::struct32};
  std::vector<workload> workloads;
  const auto append = [&workloads](const std::vector<workload>& more) {
    workloads.insert(workloads.end(), more.begin(), more.end());
  };
  switch (s) {
    case suite::real_keys:
      for (const key_kind key : real_key_kinds) {
        append(workload::product(std::array{key}, u64_values, enum_traits<key_kind>::sizes(key),
                                 inserts_hits_and_misses));
      }
      break;
    case suite::tiny:
      workloads =
          workload::product(std::array{key_kind::json_string}, struct_values,
                            std::array{map_size::four_entry_maps, map_size::thirty_two_entry_maps},
                            std::array{operation::build_many, operation::get_many, operation::iterate});
      append(workload::product(std::array{key_kind::json_object_keys}, struct_values,
                               std::array{map_size::real_json_objects},
                               enum_traits<operation>::on_many_maps));
      break;
    case suite::small:
      workloads = workload::product(std::array{key_kind::u64_random, key_kind::crate_string}, u64_values,
                                    std::array{map_size::one_thousand}, enum_traits<operation>::on_one_map);
      append(workload::product(std::array{key_kind::crate_names, key_kind::kernel_paths}, u64_values,
                               std::array{map_size::one_thousand}, inserts_hits_and_misses));
      break;
    case suite::ci:
      workloads = workload::product(std::array{key_kind::u64_random}, u64_values,
                                    std::array{map_size::one_thousand, map_size::one_million},
                                    enum_traits<operation>::on_one_map);
      append(workload::product(std::array{key_kind::u64_random}, u64_values,
                               std::array{map_size::ten_million}, inserts_hits_and_misses));
      append(workload::product(std::array{key_kind::crate_string}, u64_values,
                               std::array{map_size::one_thousand}, inserts_hits_and_misses));
      append(workload::product(std::array{key_kind::json_string}, u64_values,
                               std::array{map_size::one_million}, inserts_hits_and_misses));
      break;
    case suite::cpp_maps:
      workloads = workload::product(std::array{key_kind::u64_random}, u64_values,
                                    std::array{map_size::one_million}, enum_traits<operation>::on_one_map);
      append(workload::product(std::array{key_kind::u64_random}, u64_values,
                               std::array{map_size::ten_million}, inserts_hits_and_misses));
      append(workload::product(std::array{key_kind::json_string}, u64_values,
                               std::array{map_size::one_million}, inserts_hits_and_misses));
      break;
  }
  return workloads;
}

std::vector<suite_row> suite_row::select(std::span<const suite> suites, const cell_filter& filter) {
  std::vector<suite_row> rows;
  for (const suite s : suites) {
    for (const workload& work : enum_traits<suite>::workloads(s)) {
      const bool listed = std::ranges::any_of(rows, [&](const suite_row& row) { return row.work() == work; });
      if (filter.admits(work) && !listed) {
        rows.push_back(suite_row(s, work));
      }
    }
  }
  return rows;
}

}  // namespace bbtree_bench
