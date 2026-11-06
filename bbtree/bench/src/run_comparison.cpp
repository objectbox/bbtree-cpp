#include "run_comparison.hpp"

#include <algorithm>
#include <format>
#include <fstream>

#include "map_kind.hpp"
#include "report.hpp"
#include "text_file.hpp"
#include "text_lines.hpp"

namespace bbtree_bench {

std::vector<metric> metric::all() {
  std::vector<metric> metrics{time()};
  for (const perf_event event : perf_events::all) {
    metrics.push_back(count(event));
  }
  return metrics;
}

std::string_view metric::column() const noexcept {
  return event_ ? perf_events::column(*event_) : run_log::time_column;
}

std::string_view metric::title() const noexcept { return event_ ? perf_events::title(*event_) : "ns"; }

std::string recorded_value::text() const {
  switch (state_) {
    case state::measured:
      return report_number::text(value_);
    case state::failed:
      return std::string(run_log::failed_status);
    case state::absent:
      break;
  }
  return "–";
}

std::string recorded_value::change_from(const recorded_value& base) const {
  if (base.state_ == state::failed || state_ == state::failed) {
    return std::string(run_log::failed_status);
  }
  if (base.state_ == state::measured && state_ == state::measured) {
    if (base.value_ > 0.0) {
      return std::format("{:+.1f}%", (value_ / base.value_ - 1.0) * 100.0);
    }
    if (value_ == 0.0) {
      return "0";
    }
    return "0 → " + report_number::text(value_);
  }
  return "–";
}

std::string recorded_value::ratio_to(const recorded_value& baseline) const {
  if (state_ == state::measured && baseline.state_ == state::measured && baseline.value_ > 0.0) {
    return std::format("{:.2f}×", value_ / baseline.value_);
  }
  if (state_ == state::failed || baseline.state_ == state::failed) {
    return std::string(run_log::failed_status);
  }
  return "–";
}

recorded_run recorded_run::read(const std::filesystem::path& path) {
  const std::optional<std::string> text = text_file::read(path);
  if (!text) {
    throw bench_error(std::format("reading {} failed", path.string()));
  }
  try {
    return parse(*text);
  } catch (const bench_error& problem) {
    throw bench_error(std::format("{}: {}", path.string(), problem.what()));
  }
}

recorded_run recorded_run::parse(std::string_view text) {
  recorded_run run;
  bool header = true;
  text_lines::for_each(text, [&](std::string_view line) {
    std::vector<std::string> fields;
    for (const std::string_view field : text_lines::split(line, ',')) {
      fields.emplace_back(field);
    }
    if (header) {
      run.columns_ = std::move(fields);
      header = false;
    } else {
      run.rows_.push_back(std::move(fields));
    }
  });
  if (header) {
    throw bench_error("no header");
  }
  for (const std::string_view column :
       {run_log::benchmark_column, run_log::map_column, run_log::status_column}) {
    if (!run.column_index(column)) {
      throw bench_error(std::format("no `{}` column", column));
    }
  }
  return run;
}

std::optional<std::size_t> recorded_run::column_index(std::string_view name) const {
  const auto found = std::ranges::find(columns_, name);
  if (found == columns_.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - columns_.begin());
}

std::optional<std::string_view> recorded_run::field(const std::vector<std::string>& row,
                                                    std::string_view column) const {
  const std::optional<std::size_t> index = column_index(column);
  if (!index || *index >= row.size()) {
    return std::nullopt;
  }
  return row[*index];
}

std::vector<std::string> recorded_run::benchmarks() const {
  std::vector<std::pair<std::string, std::string>> benchmarks;
  for (const std::vector<std::string>& row : rows_) {
    const std::string benchmark(field(row, run_log::benchmark_column).value_or(""));
    const bool listed = std::ranges::any_of(
        benchmarks, [&](const auto& listed_benchmark) { return listed_benchmark.first == benchmark; });
    if (!listed) {
      benchmarks.emplace_back(benchmark, std::string(field(row, run_log::size_column).value_or("")));
    }
  }
  const std::string_view leading_size = named<map_size>::name(map_size::ten_million);
  std::ranges::stable_sort(benchmarks, [&](const auto& left, const auto& right) {
    return (left.second == leading_size) > (right.second == leading_size);
  });
  std::vector<std::string> names;
  for (auto& [benchmark, size] : benchmarks) {
    names.push_back(std::move(benchmark));
  }
  return names;
}

recorded_value recorded_run::value(std::string_view benchmark, std::string_view map,
                                   const metric& measured) const {
  std::vector<const std::vector<std::string>*> rows;
  for (const std::vector<std::string>& row : rows_) {
    if (field(row, run_log::benchmark_column) == benchmark && field(row, run_log::map_column) == map) {
      rows.push_back(&row);
    }
  }
  const auto has_status = [&](const std::vector<std::string>& row, std::string_view status) {
    return field(row, run_log::status_column) == status;
  };
  if (std::ranges::any_of(rows, [&](const auto* row) { return has_status(*row, run_log::failed_status); })) {
    return recorded_value::failed();
  }
  std::vector<double> values;
  for (const std::vector<std::string>* row : rows) {
    if (!has_status(*row, run_log::measured_status)) {
      continue;
    }
    if (const std::optional<std::string_view> text = field(*row, measured.column())) {
      if (const std::optional<double> number = text_lines::parse_double(*text)) {
        values.push_back(*number);
      }
    }
  }
  const std::optional<double> median = statistics::of(statistic::median, std::move(values));
  return median ? recorded_value::measured(*median) : recorded_value::absent();
}

run_comparison run_comparison::load(const comparison_request& request) {
  std::optional<recorded_run> base;
  if (request.base()) {
    base = recorded_run::read(*request.base());
  }
  return run_comparison(recorded_run::read(request.head()), std::move(base));
}

std::string run_comparison::markdown() const {
  const std::string_view unsync = named<map_kind>::name(map_kind::unsync);
  const std::string_view std_btree = named<map_kind>::name(map_kind::std_btree);
  std::string text;
  if (base_) {
    text += "#### bbtree-unsync, change from base to head\n\n";
    text += metric_table([&](std::string_view benchmark, const metric& measured) {
      return head_.value(benchmark, unsync, measured).change_from(base_->value(benchmark, unsync, measured));
    });
  }
  text += "#### bbtree-unsync ÷ std-btree, at head (lower is better)\n\n";
  text += metric_table([&](std::string_view benchmark, const metric& measured) {
    return head_.value(benchmark, unsync, measured).ratio_to(head_.value(benchmark, std_btree, measured));
  });
  text += "<details><summary>Every number</summary>\n\n";
  for (const metric& measured : metric::all()) {
    text += std::format("#### {}\n\n{}", measured.title(), values_table(measured));
  }
  text += "</details>\n\n";
  text +=
      "Per operation, medians over repetitions. ns is wall time; the counts are perf's, in user space, of "
      "the "
      "timed regions on the benchmark's main thread. Lines from memory are L1d fills from DRAM on AMD, and "
      "L3 "
      "misses on Intel. std-btree is Rust's `BTreeMap` in a Rust run, `std::map` in a C++ run.\n";
  return text;
}

std::string run_comparison::metric_table(
    const std::function<std::string(std::string_view, const metric&)>& cell) const {
  const std::vector<metric> metrics = metric::all();
  std::string titles;
  std::string alignments;
  for (const metric& measured : metrics) {
    titles += titles.empty() ? std::string(measured.title()) : " | " + std::string(measured.title());
    alignments += "--:|";
  }
  std::string text = std::format("| benchmark | {} |\n|---|{}\n", titles, alignments);
  for (const std::string& benchmark : head_.benchmarks()) {
    std::string cells;
    for (const metric& measured : metrics) {
      cells += cells.empty() ? cell(benchmark, measured) : " | " + cell(benchmark, measured);
    }
    text += std::format("| {} | {} |\n", benchmark, cells);
  }
  text += '\n';
  return text;
}

std::string run_comparison::values_table(const metric& measured) const {
  std::vector<std::pair<std::string_view, const recorded_run*>> runs;
  if (base_) {
    runs.emplace_back("base", &*base_);
  }
  runs.emplace_back("head", &head_);
  const std::array maps{map_kind::unsync, map_kind::std_btree};
  std::string titles;
  std::string alignments;
  for (const map_kind map : maps) {
    for (const auto& [run_name, run] : runs) {
      const std::string title = std::format("{} {}", named<map_kind>::name(map), run_name);
      titles += titles.empty() ? title : " | " + title;
      alignments += "--:|";
    }
  }
  std::string text = std::format("| benchmark | {} |\n|---|{}\n", titles, alignments);
  for (const std::string& benchmark : head_.benchmarks()) {
    std::string cells;
    for (const map_kind map : maps) {
      for (const auto& [run_name, run] : runs) {
        const std::string value = run->value(benchmark, named<map_kind>::name(map), measured).text();
        cells += cells.empty() ? value : " | " + value;
      }
    }
    text += std::format("| {} | {} |\n", benchmark, cells);
  }
  text += '\n';
  return text;
}

}  // namespace bbtree_bench
