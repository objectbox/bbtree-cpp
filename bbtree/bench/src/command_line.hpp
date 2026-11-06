// The command line: suite runs with filters; `compare`, which reports on two
// runs' CSVs; `verify-data` and `fetch`, which manage the datasets;
// `self-check`, which checks that the generators make the Rust bench's keys;
// and `measure`, the internal command that the suite runner starts each
// child process with: the Rust bench's `command_line.rs`.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cell_filter.hpp"
#include "map_size.hpp"
#include "suites.hpp"
#include "workload.hpp"

namespace bbtree_bench {

// Walks the arguments, handing out flag values.
class argument_cursor {
 public:
  explicit argument_cursor(std::span<const std::string> arguments) noexcept : arguments_(arguments) {}

  std::optional<std::string_view> next_argument() noexcept {
    if (next_ == arguments_.size()) {
      return std::nullopt;
    }
    return arguments_[next_++];
  }

  // The argument after `flag`; throws if there is none.
  std::string_view value_of(std::string_view flag);

 private:
  std::span<const std::string> arguments_;
  std::size_t next_ = 0;
};

// `bbtree-bench <suite>... [filters] [options]`.
class suite_request {
 public:
  static suite_request parse(std::span<const std::string> arguments);

  std::span<const suite> suites() const noexcept { return suites_; }
  const cell_filter& filter() const noexcept { return filter_; }
  std::size_t repetitions() const noexcept { return repetitions_; }
  scale run_scale() const noexcept { return scale_; }
  bool perf() const noexcept { return perf_; }
  bool list_only() const noexcept { return list_only_; }
  const std::filesystem::path& output_directory() const noexcept { return output_directory_; }
  const std::string& command_line() const noexcept { return command_line_; }

 private:
  suite_request() = default;

  std::vector<suite> suites_;
  cell_filter filter_;
  std::size_t repetitions_ = 1;
  scale scale_ = scale::full;
  bool perf_ = false;
  bool list_only_ = false;
  std::filesystem::path output_directory_;
  std::string command_line_;
};

// `bbtree-bench compare --head CSV [--base CSV]`.
class comparison_request {
 public:
  static constexpr std::string_view command = "compare";

  static comparison_request parse(std::span<const std::string> arguments);

  const std::filesystem::path& head() const noexcept { return head_; }
  const std::optional<std::filesystem::path>& base() const noexcept { return base_; }

 private:
  comparison_request(std::filesystem::path head, std::optional<std::filesystem::path> base)
      : head_(std::move(head)), base_(std::move(base)) {}

  std::filesystem::path head_;
  std::optional<std::filesystem::path> base_;
};

// `bbtree-bench measure <cell> [--quick] [--perf-control CONTROL,ACK]`:
// measures one cell in this process, for the suite runner.
class cell_request {
 public:
  static constexpr std::string_view command = "measure";

  // The arguments that make a child process measure `measured`.
  static std::vector<std::string> arguments(const cell& measured, scale run_scale,
                                            const std::optional<std::string>& perf_control);

  static cell_request parse(std::span<const std::string> arguments);

  const cell& measured() const noexcept { return cell_; }
  scale run_scale() const noexcept { return scale_; }
  const std::optional<std::string>& perf_control() const noexcept { return perf_control_; }

 private:
  cell_request(cell measured, scale run_scale, std::optional<std::string> perf_control)
      : cell_(measured), scale_(run_scale), perf_control_(std::move(perf_control)) {}

  cell cell_;
  scale scale_;
  std::optional<std::string> perf_control_;
};

// What the command line asks for.
class invocation {
 public:
  struct help {};
  // `verify-data`: hashes the datasets against the manifest.
  struct verify_data {};
  // `fetch`: installs the datasets and verifies them.
  struct fetch_data {};
  // `self-check`: checks the generators against values pinned from the
  // Rust bench.
  struct self_check {};

  using request = std::variant<help, verify_data, fetch_data, self_check, suite_request, comparison_request,
                               cell_request>;

  static request parse(std::span<const std::string> arguments);

  static std::string usage();

 private:
  // `made`, for a command that takes no arguments.
  template <class Command>
  static request without_arguments(std::span<const std::string> arguments);
};

}  // namespace bbtree_bench
