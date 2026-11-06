// `compare`: a markdown report of how bbtree's unsync map changed between two
// suite runs, such as a pull request's base and head, or the Rust bench's run
// and this one's, and how it stands against the std B-tree (`std-btree`),
// from the runs' CSVs: the Rust bench's `run_comparison.rs`. Each run may
// come from its own build of either bench: a column that one run lacks shows
// as missing.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "command_line.hpp"
#include "perf_counters.hpp"

namespace bbtree_bench {

// A number the report compares, per operation: time, or a perf count.
class metric {
 public:
  static metric time() noexcept { return metric(std::nullopt); }
  static metric count(perf_event event) noexcept { return metric(event); }

  static std::vector<metric> all();

  std::string_view column() const noexcept;
  std::string_view title() const noexcept;

 private:
  explicit metric(std::optional<perf_event> event) noexcept : event_(event) {}

  std::optional<perf_event> event_;
};

// What a run recorded of one metric of one cell.
class recorded_value {
 public:
  // The median over the cell's repetitions.
  static recorded_value measured(double value) noexcept { return recorded_value(state::measured, value); }
  static recorded_value failed() noexcept { return recorded_value(state::failed, 0); }
  // Not run (n/a, missing data), not in the run, or not counted.
  static recorded_value absent() noexcept { return recorded_value(state::absent, 0); }

  std::string text() const;

  // The change from `base` to this, in percent; from none, the new value.
  std::string change_from(const recorded_value& base) const;

  // This as a multiple of `baseline`.
  std::string ratio_to(const recorded_value& baseline) const;

 private:
  enum class state { measured, failed, absent };

  recorded_value(state recorded, double value) noexcept : state_(recorded), value_(value) {}

  state state_;
  double value_;
};

// A suite run, read back from its CSV.
class recorded_run {
 public:
  static recorded_run read(const std::filesystem::path& path);
  static recorded_run parse(std::string_view text);

  // The benchmarks: those at 10M first, the size the report leads with, then
  // the rest, each in the order the run lists them.
  std::vector<std::string> benchmarks() const;

  // `measured` of `map` in `benchmark`.
  recorded_value value(std::string_view benchmark, std::string_view map, const metric& measured) const;

 private:
  std::optional<std::size_t> column_index(std::string_view name) const;
  std::optional<std::string_view> field(const std::vector<std::string>& row, std::string_view column) const;

  std::vector<std::string> columns_;
  std::vector<std::vector<std::string>> rows_;
};

// The report: bbtree's unsync map's change from the base run to the head
// run, the map against the std B-tree in the head run, and every number.
// Without a base run, the latter two.
class run_comparison {
 public:
  static run_comparison load(const comparison_request& request);

  std::string markdown() const;

 private:
  run_comparison(recorded_run head, std::optional<recorded_run> base)
      : head_(std::move(head)), base_(std::move(base)) {}

  // A row per benchmark of the head run and a column per metric, each cell
  // `cell(benchmark, metric)`.
  std::string metric_table(const std::function<std::string(std::string_view, const metric&)>& cell) const;

  // `measured` of both maps, in each run.
  std::string values_table(const metric& measured) const;

  recorded_run head_;
  std::optional<recorded_run> base_;
};

}  // namespace bbtree_bench
