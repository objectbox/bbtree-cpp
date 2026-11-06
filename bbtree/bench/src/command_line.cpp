#include "command_line.hpp"

#include <charconv>
#include <format>

#include "datasets.hpp"
#include "text_lines.hpp"

namespace bbtree_bench {

std::string_view argument_cursor::value_of(std::string_view flag) {
  const std::optional<std::string_view> value = next_argument();
  if (!value) {
    throw bench_error(std::format("{} needs a value", flag));
  }
  return *value;
}

bool cell_filter::admits_map(map_kind map) const noexcept {
  if (maps_.empty()) {
    return !enum_traits<map_kind>::is_opt_in(map);
  }
  return named<map_kind>::listed(maps_, map);
}

bool cell_filter::admits(const workload& work) const noexcept {
  return list_admits(keys_, work.key()) && list_admits(values_, work.value()) &&
         list_admits(sizes_, work.size()) && list_admits(operations_, work.op());
}

void cell_filter::parse_flag(std::string_view flag, argument_cursor& cursor) {
  const auto extend = [](auto& list, const auto& more) { list.insert(list.end(), more.begin(), more.end()); };
  if (flag != "--map" && flag != "--key" && flag != "--value" && flag != "--size" && flag != "--op") {
    throw bench_error(std::format("unknown option {}", flag));
  }
  const std::string_view names = cursor.value_of(flag);
  if (flag == "--map") {
    extend(maps_, named<map_kind>::from_name_list(names));
  } else if (flag == "--key") {
    extend(keys_, named<key_kind>::from_name_list(names));
  } else if (flag == "--value") {
    extend(values_, named<value_kind>::from_name_list(names));
  } else if (flag == "--size") {
    extend(sizes_, named<map_size>::from_name_list(names));
  } else {
    extend(operations_, named<operation>::from_name_list(names));
  }
}

template <class E>
E cell_filter::only(const std::vector<E>& list, std::string_view flag) {
  if (list.size() != 1) {
    throw bench_error(std::format("{} takes exactly one name here", flag));
  }
  return list.front();
}

cell cell_filter::only_cell() const {
  const key_kind key = only(keys_, "--key");
  const value_kind value = only(values_, "--value");
  const map_size size = only(sizes_, "--size");
  const operation op = only(operations_, "--op");
  return cell(only(maps_, "--map"), workload(key, value, size, op));
}

suite_request suite_request::parse(std::span<const std::string> arguments) {
  suite_request request;
  request.output_directory_ = std::filesystem::path(BBTREE_BENCH_SOURCE_DIR) / "results";
  for (const std::string& argument : arguments) {
    request.command_line_ += request.command_line_.empty() ? argument : " " + argument;
  }
  std::optional<std::size_t> repetitions;
  argument_cursor cursor(arguments);
  while (const std::optional<std::string_view> argument = cursor.next_argument()) {
    if (*argument == "--quick") {
      request.scale_ = scale::quick;
    } else if (*argument == "--perf") {
      request.perf_ = true;
    } else if (*argument == "--list") {
      request.list_only_ = true;
    } else if (*argument == "--reps") {
      const std::string_view count = cursor.value_of(*argument);
      const std::optional<std::uint64_t> parsed = text_lines::parse_u64(count);
      if (!parsed) {
        throw bench_error(std::format("--reps takes a count, not `{}`", count));
      }
      repetitions = static_cast<std::size_t>(*parsed);
    } else if (*argument == "--out") {
      request.output_directory_ = std::filesystem::path(std::string(cursor.value_of(*argument)));
    } else if (*argument == "all") {
      request.suites_.insert(request.suites_.end(), enum_traits<suite>::all.begin(),
                             enum_traits<suite>::all.end());
    } else if (argument->starts_with("--")) {
      request.filter_.parse_flag(*argument, cursor);
    } else {
      request.suites_.push_back(named<suite>::from_name(*argument));
    }
  }
  if (request.suites_.empty()) {
    throw bench_error("name at least one suite, or `all`");
  }
  if (repetitions == std::size_t{0}) {
    throw bench_error("--reps must be at least 1");
  }
  request.repetitions_ = repetitions.value_or(1);
  return request;
}

comparison_request comparison_request::parse(std::span<const std::string> arguments) {
  std::optional<std::filesystem::path> head;
  std::optional<std::filesystem::path> base;
  argument_cursor cursor(arguments);
  while (const std::optional<std::string_view> argument = cursor.next_argument()) {
    std::optional<std::filesystem::path>* run = nullptr;
    if (*argument == "--head") {
      run = &head;
    } else if (*argument == "--base") {
      run = &base;
    } else {
      throw bench_error(std::format("{} takes --head and --base", command));
    }
    *run = std::filesystem::path(std::string(cursor.value_of(*argument)));
  }
  if (!head) {
    throw bench_error(std::format("{} needs --head", command));
  }
  return comparison_request(std::move(*head), std::move(base));
}

std::vector<std::string> cell_request::arguments(const cell& measured, scale run_scale,
                                                 const std::optional<std::string>& perf_control) {
  const workload& work = measured.work();
  std::vector<std::string> arguments{
      std::string(command),
      "--map",
      std::string(named<map_kind>::name(measured.map())),
      "--key",
      std::string(named<key_kind>::name(work.key())),
      "--value",
      std::string(named<value_kind>::name(work.value())),
      "--size",
      std::string(named<map_size>::name(work.size())),
      "--op",
      std::string(named<operation>::name(work.op())),
  };
  if (run_scale == scale::quick) {
    arguments.emplace_back("--quick");
  }
  if (perf_control) {
    arguments.emplace_back("--perf-control");
    arguments.push_back(*perf_control);
  }
  return arguments;
}

cell_request cell_request::parse(std::span<const std::string> arguments) {
  cell_filter filter;
  scale run_scale = scale::full;
  std::optional<std::string> perf_control;
  argument_cursor cursor(arguments);
  while (const std::optional<std::string_view> argument = cursor.next_argument()) {
    if (*argument == "--quick") {
      run_scale = scale::quick;
    } else if (*argument == "--perf-control") {
      perf_control = std::string(cursor.value_of(*argument));
    } else {
      filter.parse_flag(*argument, cursor);
    }
  }
  return cell_request(filter.only_cell(), run_scale, std::move(perf_control));
}

template <class Command>
invocation::request invocation::without_arguments(std::span<const std::string> arguments) {
  if (arguments.size() > 1) {
    throw bench_error(std::format("{} takes no arguments", arguments.front()));
  }
  return Command{};
}

invocation::request invocation::parse(std::span<const std::string> arguments) {
  if (arguments.empty()) {
    return help{};
  }
  const std::string_view command = arguments.front();
  if (command == "-h" || command == "--help" || command == "help") {
    return help{};
  }
  if (command == "verify-data") {
    return without_arguments<verify_data>(arguments);
  }
  if (command == "fetch") {
    return without_arguments<fetch_data>(arguments);
  }
  if (command == "self-check") {
    return without_arguments<self_check>(arguments);
  }
  if (command == comparison_request::command) {
    return comparison_request::parse(arguments.subspan(1));
  }
  if (command == cell_request::command) {
    return cell_request::parse(arguments.subspan(1));
  }
  return suite_request::parse(arguments);
}

std::string invocation::usage() {
  return std::format(
      R"(bbtree++'s benchmarks, a port of the Rust bbtree bench: the same cells, keys
and CSV columns, so that `compare` reports a C++ run against a Rust one. Every
cell (map x key x value x size x operation) runs in a fresh process, once per
repetition.

usage: bbtree-bench <suite>... [filters] [options]
       bbtree-bench-memory <suite>... [filters] [options]
       bbtree-bench compare --head CSV [--base CSV]
       bbtree-bench verify-data
       bbtree-bench fetch
       bbtree-bench self-check

The memory binary reports the memory held by the maps that build operations
make, instead of time.

compare prints a markdown report of a timing run's CSV: bbtree-unsync against
std-btree, time and perf counts per operation, and with --base, its change
from that run. The runs may come from either bench, C++ or Rust.

self-check checks that the key, value and order generators make the Rust
bench's sequences, against digests pinned from it.

suites: {}, or all

Real-world key kinds read their datasets from ${}, or else
$XDG_CACHE_HOME/bbtree-bench, or else ~/.cache/bbtree-bench. Their cells
check only that each file has its pinned size, and are reported as missing
data when one doesn't.
  verify-data  hashes every dataset file against the manifest
  fetch        downloads the datasets from their GitHub release with gh and
               zstd, derives Common Crawl's URLs from the crawl's index with
               python3, then verifies them

filters, comma-separated (default: everything in the suites):
  --map    {}
  --key    {}
  --value  {}
  --size   {}
  --op     {}

options:
  --reps N   repetitions per cell (default 1; more for cells shown to be
             noisy)
  --quick    10x smaller sizes and operation counts, for CI and development
  --perf     count cycles, instructions, branches, branch misses, L1d misses
             and cache lines from memory, of the timed regions on the main
             thread, with `perf stat`
  --list     print the selected cells and exit
  --out DIR  where to write the CSV and the summary (default: bbtree/bench/results))",
      named<suite>::all_names(), data_directory::variable, named<map_kind>::all_names(),
      named<key_kind>::all_names(), named<value_kind>::all_names(), named<map_size>::all_names(),
      named<operation>::all_names());
}

}  // namespace bbtree_bench
