// A suite run's results: a CSV row per run, and a markdown summary under a
// header that records where and how the numbers were made: the Rust bench's
// `report.rs`. The CSV's columns are the Rust bench's, so that `compare`
// reads either bench's runs.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cell_runner.hpp"
#include "command_line.hpp"
#include "memory.hpp"
#include "perf_counters.hpp"
#include "suites.hpp"

namespace bbtree_bench {

// Where and how a run's numbers were made, and how long it took on how busy
// a machine.
class run_header {
 public:
  run_header(const suite_request& request, run_mode mode);

  // Records the run's wall time and the load average at its end, once every
  // cell has run.
  void record_end();

  // Records the events that `--perf` leaves out on this CPU, which the
  // settings then name.
  void record_left_out(std::span<const perf_event> events);

  // The start time, safe for file names: `20261007T143810Z`.
  std::string file_stamp() const;

  std::string markdown() const;

 private:
  // The widest vector extension the binary was built for, which the maps'
  // searches may use: the bench preset builds for the host CPU.
  static std::string_view vector_extensions() noexcept;

  // The load average over the last 1, 5 and 15 minutes, which shows whether
  // other work shared the machine.
  static std::string load_average();

  // `git rev-parse HEAD` in this repository, marked dirty if anything
  // tracked or unignored has changed.
  static std::string git_commit();

  static std::string cpu_model();

  static std::string compiler();

  // When a run ended.
  struct run_end {
    std::chrono::steady_clock::duration wall_time;
    std::string load_average;
  };

  std::string started_at_;
  std::string commit_;
  std::string cpu_;
  std::string_view allocator_;
  std::string settings_;
  std::string command_line_;
  std::chrono::steady_clock::time_point started_;
  std::string load_average_at_start_;
  std::optional<run_end> end_;
};

// How one run of a cell ended.
class run_outcome {
 public:
  // The map lacks the operation, so the cell doesn't run.
  struct not_applicable {};
  // A data file the keys are read from is missing or has the wrong size, so
  // the cell doesn't run.
  struct missing_data {};
  struct failed {
    std::string reason;
  };
  struct measured {
    cell_measurement measurement;
    std::optional<perf_counts> counts;
  };

  using outcome = std::variant<not_applicable, missing_data, failed, measured>;

  run_outcome(outcome result) : outcome_(std::move(result)) {}  // NOLINT(google-explicit-constructor)

  static constexpr std::string_view missing_data_text = "missing data (run verify-data)";

  std::optional<double> summary_value() const noexcept;

  // A few words for the progress line.
  std::string brief() const;

  const outcome& result() const noexcept { return outcome_; }

 private:
  outcome outcome_;
};

// One run of one cell, or the single record of a cell that doesn't run:
// `n/a`, or missing data.
class run_record {
 public:
  // The record of a cell that doesn't run, for `outcome`'s reason.
  static run_record not_run(const suite_row& row, map_kind map, run_outcome outcome);

  static run_record run(const suite_row& row, map_kind map, std::size_t repetition, run_outcome outcome);

  bool is_of(const suite_row& row, map_kind map) const noexcept {
    return map_ == map && row_.work() == row.work();
  }

  const std::optional<std::size_t>& repetition() const noexcept { return repetition_; }
  const run_outcome& outcome() const noexcept { return outcome_; }

  std::string csv_row() const;

 private:
  run_record(const suite_row& row, map_kind map, std::optional<std::size_t> repetition, run_outcome outcome)
      : row_(row), map_(map), repetition_(repetition), outcome_(std::move(outcome)) {}

  // The fields of `run_log`'s measured columns.
  static std::array<std::string, 8> measured_fields(const cell_measurement& measurement);

  // The perf counts per operation, in `perf_events::all` order, to six
  // decimals: one event in a million operations, which rare events, such as
  // cache lines from memory on small maps, come down to. Empty for an event
  // left out.
  static std::array<std::string, perf_events::all.size()> perf_fields(
      const cell_measurement& measurement, const std::optional<perf_counts>& counts);

  suite_row row_;
  map_kind map_;
  std::optional<std::size_t> repetition_;
  run_outcome outcome_;
};

// The CSV of every run, written as runs finish, so an interrupted suite
// keeps what it measured.
class run_log {
 public:
  // The columns that `compare` reads, and the statuses it tells apart.
  static constexpr std::string_view benchmark_column = "benchmark";
  static constexpr std::string_view map_column = "map";
  static constexpr std::string_view size_column = "size";
  static constexpr std::string_view status_column = "status";
  static constexpr std::string_view time_column = "ns_per_op";
  static constexpr std::string_view measured_status = "ok";
  static constexpr std::string_view failed_status = "failed";

  static constexpr std::array<std::string_view, 8> identity_columns{
      benchmark_column, map_column, "key", "value", size_column, "operation", "repetition", status_column,
  };

  // Timing runs fill the first three and the last; memory runs the rest.
  static constexpr std::array<std::string_view, 8> measured_columns{
      "operations", "elapsed_ns",      time_column,        "entries",
      "map_bytes",  "bytes_per_entry", "rss_growth_bytes", "peak_rss_bytes",
  };

  explicit run_log(std::filesystem::path path);

  void append(const run_record& record) { write_line(record.csv_row()); }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  void write_line(std::string_view line);

  std::filesystem::path path_;
  std::ofstream file_;
};

// Which number of a cell's runs a summary table shows.
enum class statistic { median, minimum };

class statistics {
 public:
  static std::optional<double> of(statistic which, std::vector<double> values);
};

// A measured value as reports print it: three significant digits, as in
// `0.00312`, `2.31`, `23.1` and `231`, and whole numbers above that.
class report_number {
 public:
  static std::string text(double value);
};

// The markdown summary: one row per workload, one column per map, and
// bbtree's unsync map against the std B-tree.
class summary {
 public:
  summary(const run_header& header, std::span<const suite_row> rows, std::span<const map_kind> maps,
          std::span<const run_record> records, run_mode mode) noexcept
      : header_(header), rows_(rows), maps_(maps), records_(records), mode_(mode) {}

  std::string markdown() const;

 private:
  bool has_repetitions() const noexcept;
  std::string table(std::string_view title, statistic which) const;
  std::optional<double> value(const suite_row& row, map_kind map, statistic which) const;
  std::string cell_text(const suite_row& row, map_kind map, statistic which) const;
  std::optional<std::string_view> comparison_title() const noexcept;
  std::string comparison_text(const suite_row& row, statistic which) const;

  template <class Outcome>
  bool any_outcome(const suite_row& row, map_kind map) const noexcept;

  const run_header& header_;
  std::span<const suite_row> rows_;
  std::span<const map_kind> maps_;
  std::span<const run_record> records_;
  run_mode mode_;
};

}  // namespace bbtree_bench
