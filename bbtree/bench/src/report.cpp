#include "report.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "child_process.hpp"
#include "text_file.hpp"
#include "text_lines.hpp"

namespace bbtree_bench {

run_header::run_header(const suite_request& request, run_mode mode)
    : allocator_(mode.allocator_description()),
      command_line_(request.command_line()),
      started_(std::chrono::steady_clock::now()),
      load_average_at_start_(load_average()) {
  child_command date("date");
  date.arg("-u").arg("+%Y-%m-%dT%H:%M:%SZ");
  started_at_ = date.trimmed_output_if_success().value_or("unknown");
  commit_ = git_commit();
  cpu_ = cpu_model();
  const std::string_view run_scale =
      request.run_scale() == scale::full ? "full size" : "--quick (10× smaller)";
  const std::string_view perf = request.perf() ? ", perf counters on (user space)" : "";
  settings_ = std::format("{} mode, {}, {} repetition(s) per cell{}", mode.name(), run_scale,
                          request.repetitions(), perf);
}

std::string_view run_header::vector_extensions() noexcept {
#if defined(__AVX512F__)
  return "AVX-512";
#elif defined(__AVX2__)
  return "AVX2";
#else
  return "SSE2 (x86-64 baseline)";
#endif
}

void run_header::record_end() { end_ = run_end{std::chrono::steady_clock::now() - started_, load_average()}; }

void run_header::record_left_out(std::span<const perf_event> events) {
  std::string titles;
  for (const perf_event event : events) {
    titles += titles.empty() ? "" : ", ";
    titles += perf_events::title(event);
  }
  if (!titles.empty()) {
    settings_ += std::format("; not counted on this CPU: {}", titles);
  }
}

std::string run_header::file_stamp() const {
  std::string stamp;
  for (const char character : started_at_) {
    if ((character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z')) {
      stamp.push_back(character);
    }
  }
  return stamp;
}

std::string run_header::markdown() const {
  std::string text = std::format(
      "| | |\n|---|---|\n| implementation | bbtree++ (C++), {} |\n| date | {} |\n| commit | {} |\n| CPU | {} "
      "|\n"
      "| allocator | {} |\n| vector extensions | {} |\n| settings | {} |\n| command | `{}` |\n"
      "| load average (1, 5, 15 min) at start | {} |\n",
      compiler(), started_at_, commit_, cpu_, allocator_, vector_extensions(), settings_, command_line_,
      load_average_at_start_);
  if (end_) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(end_->wall_time).count();
    text += std::format("| load average at end | {} |\n| wall time | {} min {:02} s |\n", end_->load_average,
                        seconds / 60, seconds % 60);
  }
  return text;
}

std::string run_header::load_average() {
  const std::string text = text_file::read("/proc/loadavg").value_or("");
  std::vector<std::string_view> fields;
  for (const std::string_view field : text_lines::split(text_lines::trimmed(text), ' ')) {
    if (!field.empty() && fields.size() < 3) {
      fields.push_back(field);
    }
  }
  if (fields.size() != 3) {
    return "unknown";
  }
  return std::format("{}, {}, {}", fields[0], fields[1], fields[2]);
}

std::string run_header::git_commit() {
  const std::string repository = BBTREE_REPOSITORY_DIR;
  child_command rev_parse("git");
  rev_parse.arg("-C").arg(repository).arg("rev-parse").arg("HEAD");
  const std::optional<std::string> commit = rev_parse.trimmed_output_if_success();
  if (!commit) {
    return "unknown";
  }
  child_command status("git");
  status.arg("-C").arg(repository).arg("status").arg("--porcelain");
  const std::optional<std::string> changes = status.trimmed_output_if_success();
  if (changes && !changes->empty()) {
    return *commit + " (dirty)";
  }
  return *commit;
}

std::string run_header::cpu_model() {
  const std::string cpu_info = text_file::read("/proc/cpuinfo").value_or("");
  std::string model = "unknown";
  bool found = false;
  text_lines::for_each(cpu_info, [&](std::string_view line) {
    if (found || !line.starts_with("model name")) {
      return;
    }
    found = true;
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
      model = std::string(text_lines::trimmed(line.substr(colon + 1)));
    }
  });
  return model;
}

std::string run_header::compiler() {
#if defined(__clang__)
  return std::format("Clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
  return std::format("GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
  return "unknown compiler";
#endif
}

std::optional<double> run_outcome::summary_value() const noexcept {
  if (const auto* done = std::get_if<measured>(&outcome_)) {
    return done->measurement.summary_value();
  }
  return std::nullopt;
}

std::string run_outcome::brief() const {
  if (std::holds_alternative<not_applicable>(outcome_)) {
    return "n/a";
  }
  if (std::holds_alternative<missing_data>(outcome_)) {
    return std::string(missing_data_text);
  }
  if (const auto* failure = std::get_if<failed>(&outcome_)) {
    return "FAILED: " + failure->reason;
  }
  const cell_measurement& measurement = std::get<measured>(outcome_).measurement;
  if (measurement.timed()) {
    return std::format("{:.1f} ns/op", measurement.summary_value());
  }
  return std::format("{:.1f} B/entry", measurement.summary_value());
}

run_record run_record::not_run(const suite_row& row, map_kind map, run_outcome outcome) {
  bench_error::unless(std::holds_alternative<run_outcome::not_applicable>(outcome.result()) ||
                          std::holds_alternative<run_outcome::missing_data>(outcome.result()),
                      "cells that run have a record per repetition");
  return run_record(row, map, std::nullopt, std::move(outcome));
}

run_record run_record::run(const suite_row& row, map_kind map, std::size_t repetition, run_outcome outcome) {
  return run_record(row, map, repetition, std::move(outcome));
}

std::string run_record::csv_row() const {
  const workload& work = row_.work();
  std::string_view status = run_log::measured_status;
  if (std::holds_alternative<run_outcome::not_applicable>(outcome_.result())) {
    status = "n/a";
  } else if (std::holds_alternative<run_outcome::missing_data>(outcome_.result())) {
    status = "missing-data";
  } else if (std::holds_alternative<run_outcome::failed>(outcome_.result())) {
    status = run_log::failed_status;
  }
  std::vector<std::string> fields{
      work.name(),
      std::string(named<map_kind>::name(map_)),
      std::string(named<key_kind>::name(work.key())),
      std::string(named<value_kind>::name(work.value())),
      std::string(named<map_size>::name(work.size())),
      std::string(named<operation>::name(work.op())),
      repetition_ ? std::to_string(*repetition_) : std::string(),
      std::string(status),
  };
  std::array<std::string, 8> measured{};
  std::array<std::string, perf_events::all.size()> perf{};
  if (const auto* done = std::get_if<run_outcome::measured>(&outcome_.result())) {
    measured = measured_fields(done->measurement);
    perf = perf_fields(done->measurement, done->counts);
  }
  fields.insert(fields.end(), measured.begin(), measured.end());
  fields.insert(fields.end(), perf.begin(), perf.end());
  std::string row;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    row += index == 0 ? fields[index] : "," + fields[index];
  }
  return row;
}

std::array<std::string, 8> run_record::measured_fields(const cell_measurement& measurement) {
  const std::string summary_value = std::format("{:.3f}", measurement.summary_value());
  if (const cell_measurement::timing* timed = measurement.timed()) {
    return {std::to_string(timed->operations),
            std::to_string(timed->elapsed_ns),
            summary_value,
            "",
            "",
            "",
            "",
            std::to_string(timed->peak_rss_bytes)};
  }
  const cell_measurement::memory& memory = measurement.of_memory();
  return {"",
          "",
          "",
          std::to_string(memory.entries),
          std::to_string(memory.map_bytes),
          summary_value,
          std::to_string(memory.rss_growth_bytes),
          std::to_string(memory.peak_rss_bytes)};
}

std::array<std::string, perf_events::all.size()> run_record::perf_fields(
    const cell_measurement& measurement, const std::optional<perf_counts>& counts) {
  std::array<std::string, perf_events::all.size()> fields{};
  const cell_measurement::timing* timed = measurement.timed();
  if (timed == nullptr || !counts) {
    return fields;
  }
  for (std::size_t index = 0; index < perf_events::all.size(); ++index) {
    if (const std::optional<double> count =
            counts->per_operation(perf_events::all[index], timed->operations)) {
      fields[index] = std::format("{:.6f}", *count);
    }
  }
  return fields;
}

run_log::run_log(std::filesystem::path path) : path_(std::move(path)), file_(path_) {
  if (!file_) {
    throw bench_error(std::format("creating {}: cannot open", path_.string()));
  }
  std::string columns;
  const auto add = [&columns](std::string_view column) {
    columns += columns.empty() ? std::string(column) : "," + std::string(column);
  };
  for (const std::string_view column : identity_columns) {
    add(column);
  }
  for (const std::string_view column : measured_columns) {
    add(column);
  }
  for (const perf_event event : perf_events::all) {
    add(perf_events::column(event));
  }
  write_line(columns);
}

void run_log::write_line(std::string_view line) {
  file_ << line << '\n';
  file_.flush();
  if (!file_) {
    throw bench_error(std::format("writing {} failed", path_.string()));
  }
}

std::optional<double> statistics::of(statistic which, std::vector<double> values) {
  if (values.empty()) {
    return std::nullopt;
  }
  std::ranges::sort(values);
  const std::size_t middle = values.size() / 2;
  if (which == statistic::minimum) {
    return values.front();
  }
  if (values.size() % 2 == 1) {
    return values[middle];
  }
  return std::midpoint(values[middle - 1], values[middle]);
}

std::string report_number::text(double value) {
  if (std::isnan(value)) {
    return "NaN";
  }
  if (std::isinf(value)) {
    return value > 0 ? "inf" : "-inf";
  }
  const int magnitude = value == 0.0 ? 0 : static_cast<int>(std::floor(std::log10(std::abs(value))));
  const int decimals = std::max(2 - magnitude, 0);
  return std::format("{:.{}f}", value, decimals);
}

std::string summary::markdown() const {
  std::string text = std::format("# bbtree benchmark: {}\n\n{}\n", mode_.name(), header_.markdown());
  if (!mode_.is_memory()) {
    text +=
        "Time per operation: per entry for `iterate` and `bulk_build`, per scan for `range64`, per call "
        "otherwise.\n\n";
    text += table("Median ns per operation", statistic::median);
    if (has_repetitions()) {
      text += table("Fastest run, ns per operation", statistic::minimum);
    }
  } else {
    text +=
        "Bytes per entry: everything the maps hold on the heap, keys' and values' own allocations included, "
        "plus "
        "the maps' inline headers.\n\n";
    text += table("Bytes per entry", statistic::median);
  }
  return text;
}

bool summary::has_repetitions() const noexcept {
  return std::ranges::any_of(records_, [](const run_record& record) {
    return record.repetition().has_value() && *record.repetition() > 0;
  });
}

std::string summary::table(std::string_view title, statistic which) const {
  const std::optional<std::string_view> comparison = comparison_title();
  std::vector<std::string_view> columns{"benchmark"};
  for (const map_kind map : maps_) {
    columns.push_back(enum_traits<map_kind>::title(map));
  }
  if (comparison) {
    columns.push_back(*comparison);
  }
  std::string header;
  std::string alignments;
  for (std::size_t index = 0; index < columns.size(); ++index) {
    header += index == 0 ? std::string(columns[index]) : " | " + std::string(columns[index]);
    alignments += index == 0 ? "---" : "|--:";
  }
  std::string text = std::format("## {}\n\n| {} |\n|{}|\n", title, header, alignments);
  for (const suite_row& row : rows_) {
    std::string line = "| " + row.work().name();
    for (const map_kind map : maps_) {
      line += " | " + cell_text(row, map, which);
    }
    if (comparison) {
      line += " | " + comparison_text(row, which);
    }
    text += line + " |\n";
  }
  text += '\n';
  return text;
}

template <class Outcome>
bool summary::any_outcome(const suite_row& row, map_kind map) const noexcept {
  return std::ranges::any_of(records_, [&](const run_record& record) {
    return record.is_of(row, map) && std::holds_alternative<Outcome>(record.outcome().result());
  });
}

std::optional<double> summary::value(const suite_row& row, map_kind map, statistic which) const {
  std::vector<double> values;
  for (const run_record& record : records_) {
    if (record.is_of(row, map)) {
      if (const std::optional<double> measured = record.outcome().summary_value()) {
        values.push_back(*measured);
      }
    }
  }
  return statistics::of(which, std::move(values));
}

std::string summary::cell_text(const suite_row& row, map_kind map, statistic which) const {
  if (any_outcome<run_outcome::not_applicable>(row, map)) {
    return "n/a";
  }
  if (any_outcome<run_outcome::missing_data>(row, map)) {
    return std::string(run_outcome::missing_data_text);
  }
  if (any_outcome<run_outcome::failed>(row, map)) {
    return "failed";
  }
  const std::optional<double> measured = value(row, map, which);
  return measured ? report_number::text(*measured) : "–";
}

std::optional<std::string_view> summary::comparison_title() const noexcept {
  const bool both =
      named<map_kind>::listed(maps_, map_kind::unsync) && named<map_kind>::listed(maps_, map_kind::std_btree);
  if (!both) {
    return std::nullopt;
  }
  return mode_.is_memory() ? "bbtree-unsync ÷ std::map" : "bbtree-unsync speedup vs std::map";
}

std::string summary::comparison_text(const suite_row& row, statistic which) const {
  const std::optional<double> unsync = value(row, map_kind::unsync, which);
  const std::optional<double> std_btree = value(row, map_kind::std_btree, which);
  if (!unsync || !std_btree) {
    return "n/a";
  }
  const double ratio = mode_.is_memory() ? *unsync / *std_btree : *std_btree / *unsync;
  return std::format("{:.2f}×", ratio);
}

}  // namespace bbtree_bench
