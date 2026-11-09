#include "suite_runner.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <system_error>

#include "datasets.hpp"
#include "report.hpp"
#include "text_lines.hpp"

namespace bbtree_bench {

void suite_runner::run() const {
  if (request_.perf() && mode_.is_memory()) {
    throw bench_error("--perf needs the timing binary");
  }
  const std::vector<suite_row> rows = selected_rows();
  std::vector<map_kind> maps;
  for (const map_kind map : enum_traits<map_kind>::all) {
    const bool in_a_suite = std::ranges::any_of(rows, [&](const suite_row& row) {
      return named<map_kind>::listed(enum_traits<suite>::maps(row.of_suite()), map);
    });
    if (request_.filter().admits_map(map) && in_a_suite) {
      maps.push_back(map);
    }
  }
  if (rows.empty() || maps.empty()) {
    throw bench_error("no cell matches the filters");
  }
  // Every row on each of its suite's maps, split into the cells that run,
  // the cells that are `n/a` and the cells whose data is missing.
  std::vector<row_cell> applicable;
  std::vector<row_cell> not_applicable;
  for (const suite_row& row : rows) {
    for (const map_kind map : maps) {
      if (!named<map_kind>::listed(enum_traits<suite>::maps(row.of_suite()), map)) {
        continue;
      }
      const cell measured(map, row.work());
      (measured.is_applicable() ? applicable : not_applicable).emplace_back(row, measured);
    }
  }
  const std::vector<data_file> missing_files = missing_data_files(applicable);
  std::vector<row_cell> runnable;
  std::vector<row_cell> missing_data;
  for (const row_cell& entry : applicable) {
    const std::span<const data_file> needed = enum_traits<key_kind>::data_files(entry.second.work().key());
    const bool missing = std::ranges::any_of(needed, [&](data_file file) {
      return std::ranges::find(missing_files, file) != missing_files.end();
    });
    (missing ? missing_data : runnable).push_back(entry);
  }
  if (request_.list_only()) {
    print_cells(runnable, not_applicable, missing_data);
    return;
  }

  run_header header(request_, mode_);
  const std::filesystem::path& directory = request_.output_directory();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    throw bench_error(std::format("creating {}: {}", directory.string(), error.message()));
  }
  const std::string file_stem = std::format("{}-{}", header.file_stamp(), mode_.name());
  run_log log(directory / (file_stem + ".csv"));
  std::optional<perf_stat> stat;
  if (request_.perf()) {
    stat.emplace(perf_stat::prepare(directory / (".perf-" + file_stem)));
    header.record_left_out(stat->event_names().left_out());
  }
  const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) {
    throw bench_error(std::format("finding this executable: {}", error.message()));
  }

  std::vector<run_record> records;
  for (const auto& [row, measured] : not_applicable) {
    records.push_back(run_record::not_run(row, measured.map(), run_outcome(run_outcome::not_applicable{})));
  }
  for (const auto& [row, measured] : missing_data) {
    records.push_back(run_record::not_run(row, measured.map(), run_outcome(run_outcome::missing_data{})));
  }
  for (const run_record& record : records) {
    log.append(record);
  }
  const std::size_t run_count = runnable.size() * request_.repetitions();
  for (std::size_t repetition = 0; repetition < request_.repetitions(); ++repetition) {
    for (std::size_t index = 0; index < runnable.size(); ++index) {
      const auto& [row, measured] = runnable[index];
      run_outcome::outcome result = run_outcome::not_applicable{};
      try {
        auto [measurement, counts] = measure_in_child(executable, measured, stat ? &*stat : nullptr);
        result = run_outcome::measured{measurement, std::move(counts)};
      } catch (const bench_error& reason) {
        result = run_outcome::failed{reason.what()};
      }
      const run_outcome outcome(std::move(result));
      const std::size_t run_number = repetition * runnable.size() + index + 1;
      std::cerr << std::format("[{}/{}] {}: {}\n", run_number, run_count, measured.label(), outcome.brief());
      records.push_back(run_record::run(row, measured.map(), repetition, outcome));
      log.append(records.back());
    }
  }

  header.record_end();
  const std::string markdown = summary(header, rows, maps, records, mode_).markdown();
  const std::filesystem::path summary_path = directory / (file_stem + ".md");
  std::ofstream summary_file(summary_path);
  summary_file << markdown;
  summary_file.close();
  if (!summary_file) {
    throw bench_error(std::format("writing {} failed", summary_path.string()));
  }
  std::cout << markdown << '\n';
  std::cerr << std::format("wrote {} and {}\n", log.path().string(), summary_path.string());
}

std::vector<suite_row> suite_runner::selected_rows() const {
  std::vector<suite_row> rows = suite_row::select(request_.suites(), request_.filter());
  if (mode_.is_memory()) {
    std::erase_if(rows,
                  [](const suite_row& row) { return !enum_traits<operation>::builds_maps(row.work().op()); });
  }
  return rows;
}

std::vector<data_file> suite_runner::missing_data_files(std::span<const row_cell> cells) {
  std::vector<data_file> needed;
  for (const data_file file : data_files::all) {
    const bool read = std::ranges::any_of(cells, [&](const row_cell& entry) {
      return named<data_file>::listed(enum_traits<key_kind>::data_files(entry.second.work().key()), file);
    });
    if (read) {
      needed.push_back(file);
    }
  }
  if (needed.empty()) {
    return needed;
  }
  std::vector<data_file> missing;
  try {
    const data_directory directory = data_directory::from_environment();
    for (const data_file file : needed) {
      try {
        (void)directory.check_present(file);
      } catch (const bench_error& problem) {
        std::cerr << "missing data: " << problem.what() << '\n';
        missing.push_back(file);
      }
    }
  } catch (const bench_error& problem) {
    std::cerr << "missing data: " << problem.what() << '\n';
    missing = needed;
  }
  if (!missing.empty()) {
    std::cerr << "the cells that read them don't run; `bbtree-bench verify-data` checks the datasets, and "
                 "`bbtree-bench fetch` downloads them\n";
  }
  return missing;
}

void suite_runner::print_cells(std::span<const row_cell> runnable, std::span<const row_cell> not_applicable,
                               std::span<const row_cell> missing_data) const {
  for (const auto& [row, measured] : runnable) {
    std::cout << measured.label() << '\n';
  }
  for (const auto& [row, measured] : not_applicable) {
    std::cout << measured.label() << " (n/a)\n";
  }
  for (const auto& [row, measured] : missing_data) {
    std::cout << measured.label() << " (missing data)\n";
  }
  std::cout << std::format(
      "{} cells × {} repetition(s) = {} processes; {} cells n/a; {} cells missing data\n", runnable.size(),
      request_.repetitions(), runnable.size() * request_.repetitions(), not_applicable.size(),
      missing_data.size());
}

std::pair<cell_measurement, std::optional<perf_counts>> suite_runner::measure_in_child(
    const std::filesystem::path& executable, const cell& measured, const perf_stat* stat) const {
  std::optional<std::string> control;
  if (stat != nullptr) {
    control = stat->control_argument();
  }
  const std::vector<std::string> arguments = cell_request::arguments(measured, request_.run_scale(), control);
  child_command command =
      stat != nullptr ? stat->command(executable, arguments) : child_command(executable.string());
  if (stat == nullptr) {
    command.args(arguments);
  }
  child_output output = [&] {
    try {
      return command.output();
    } catch (const bench_error& failure) {
      throw bench_error(std::format("starting the child: {}", failure.what()));
    }
  }();
  text_lines::for_each(output.standard_error, [](std::string_view line) {
    if (!perf_stat::is_switch_notice(line)) {
      std::cerr << "    " << line << '\n';
    }
  });
  if (!output.status.success()) {
    throw bench_error(std::format("the child exited with {}", output.status.text()));
  }
  const cell_measurement measurement = cell_measurement::parse(text_lines::trimmed(output.standard_output));
  std::optional<perf_counts> counts;
  if (stat != nullptr) {
    counts = stat->read_counts();
  }
  return {measurement, counts};
}

}  // namespace bbtree_bench
