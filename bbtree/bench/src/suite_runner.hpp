// The suite runner: measures every selected cell, every repetition, in a
// fresh child process of this executable, then writes the CSV and the
// markdown summary: the Rust bench's `suite_runner.rs`.
//
// Repetitions are the outer loop, so a slow drift (temperature, another
// process) spreads over all cells instead of piling onto a few.
//
// Cells whose keys are read from a dataset that is missing don't run; the
// report shows them as missing data, and the rest of the suite runs.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "cell_runner.hpp"
#include "command_line.hpp"
#include "memory.hpp"
#include "perf_counters.hpp"
#include "suites.hpp"

namespace bbtree_bench {

class suite_runner {
 public:
  suite_runner(suite_request request, run_mode mode) noexcept : request_(std::move(request)), mode_(mode) {}

  void run() const;

 private:
  using row_cell = std::pair<suite_row, cell>;

  // The rows the request selects. Memory mode keeps the builds only.
  std::vector<suite_row> selected_rows() const;

  // The data files that `cells` read which aren't there with their pinned
  // size, each reported on stderr.
  static std::vector<data_file> missing_data_files(std::span<const row_cell> cells);

  void print_cells(std::span<const row_cell> runnable, std::span<const row_cell> not_applicable,
                   std::span<const row_cell> missing_data) const;

  // Runs one cell in a child process of this executable, under `perf stat`
  // if asked. The child's stderr is passed on, indented, except for perf's
  // notices.
  std::pair<cell_measurement, std::optional<perf_counts>> measure_in_child(
      const std::filesystem::path& executable, const cell& measured, const perf_stat* stat) const;

  suite_request request_;
  run_mode mode_;
};

}  // namespace bbtree_bench
