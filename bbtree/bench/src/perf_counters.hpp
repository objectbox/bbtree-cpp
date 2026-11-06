// `--perf`: hardware counters for the timed regions only: the Rust bench's
// `perf_counters.rs`.
//
// The runner starts each child under `perf stat --delay=-1 --control
// fifo:CONTROL,ACK`, which starts with the counters off, and the child's
// stopwatch switches them on and off around each timed region through the
// control FIFO. The events count user space only (`:u`), which unprivileged
// perf allows and which keeps the FIFO round trips out of the counts; kernel
// time, such as page faults, shows in ns/op but not in the counters.
//
// The counters follow the child's main thread only (`--no-inherit`), which
// runs every timed region: the setup threads would otherwise add whatever
// they still run while a region is timed. And the events form one group,
// which the CPU counts all at once or not at all, so that each count is
// exact rather than scaled up from a share of the time; a cell whose counts
// aren't exact fails.
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "child_process.hpp"

namespace bbtree_bench {

enum class perf_event {
  cycles,
  instructions,
  branches,
  branch_misses,
  l1d_load_misses,
  // Cache lines brought in from beyond the last-level cache, from DRAM
  // mostly. Each CPU vendor counts them its own way (`perf_names`), so
  // counts compare only between runs on one kind of CPU.
  lines_from_memory,
};

class perf_events {
 public:
  static constexpr std::array all{
      perf_event::cycles,        perf_event::instructions,    perf_event::branches,
      perf_event::branch_misses, perf_event::l1d_load_misses, perf_event::lines_from_memory,
  };

  // perf's names for the event, in the order to try them: the first that
  // perf counts on this CPU is the one used.
  static std::span<const std::string_view> perf_names(perf_event event) noexcept;

  // The CSV column of the count per operation.
  static std::string_view column(perf_event event) noexcept;

  // The event's name in reports.
  static std::string_view title(perf_event event) noexcept;
};

// perf's name for each of `perf_events::all` on this CPU; empty for an event
// left out.
class perf_event_names {
 public:
  using names_type = std::array<std::string_view, perf_events::all.size()>;

  explicit perf_event_names(names_type names) noexcept : names_(names) {}

  // The first name of each event that perf counts here, checked to count as
  // one group.
  static perf_event_names for_this_cpu();

  // `perf stat` counting the events as one group, on the main thread only,
  // in CSV.
  child_command perf_stat() const;

  std::string_view name(std::size_t index) const noexcept { return names_[index]; }

 private:
  // `event` alone, as `name`.
  static perf_event_names only(perf_event event, std::string_view name) noexcept;

  // Counts the events over `true`; throws unless every count is exact.
  void count_trial() const;

  names_type names_;
};

// The counts of one run, by event.
class perf_counts {
 public:
  // Parses `perf stat -x ,` output: `value,unit,event,run time,percent
  // running,...` lines, after `#` comments. Every event of `names` must be
  // there, counted all the time.
  static perf_counts parse(std::string_view text, const perf_event_names& names);

  double per_operation(perf_event event, std::uint64_t operations) const noexcept {
    return counts_[static_cast<std::size_t>(event)] / static_cast<double>(operations);
  }

 private:
  std::array<double, perf_events::all.size()> counts_{};
};

// The runner's side: the events as perf names them on this CPU, and the
// control FIFOs and the counts file, in a scratch directory that lives as
// long as this does.
class perf_stat {
 public:
  // Checks that perf counts every event exactly on this CPU, then creates
  // the scratch directory and its FIFOs. Its path must not contain a comma,
  // which separates the FIFOs in perf's arguments.
  static perf_stat prepare(std::filesystem::path directory);

  perf_stat(perf_stat&& other) noexcept;
  perf_stat& operator=(perf_stat&&) = delete;
  perf_stat(const perf_stat&) = delete;
  perf_stat& operator=(const perf_stat&) = delete;
  ~perf_stat();

  // The child's `--perf-control` value: the FIFOs it switches the counters
  // through, as `CONTROL,ACK`.
  std::string control_argument() const;

  // `perf stat` running `program` with `arguments`, counters off until the
  // program switches them on.
  child_command command(const std::filesystem::path& program,
                        const std::vector<std::string>& arguments) const;

  // The notice perf prints to stderr at every switch, which the runner drops.
  static bool is_switch_notice(std::string_view line) noexcept {
    return line == "Events enabled" || line == "Events disabled";
  }

  // The counts of the last run; throws if any isn't exact.
  perf_counts read_counts() const;

 private:
  perf_stat(std::filesystem::path directory, perf_event_names names) noexcept
      : directory_(std::move(directory)), event_names_(names) {}

  std::filesystem::path control_path() const { return directory_ / "control"; }
  std::filesystem::path ack_path() const { return directory_ / "ack"; }
  std::filesystem::path counts_path() const { return directory_ / "counts.csv"; }

  std::filesystem::path directory_;
  perf_event_names event_names_;
};

// The child's side of the control FIFOs.
class perf_control {
 public:
  // Opens the FIFOs named by `--perf-control`. perf holds both open already,
  // so neither open blocks.
  explicit perf_control(std::string_view argument);

  perf_control(perf_control&& other) noexcept;
  perf_control& operator=(perf_control&&) = delete;
  perf_control(const perf_control&) = delete;
  perf_control& operator=(const perf_control&) = delete;
  ~perf_control();

  void start_counting() { send("enable\n"); }
  void stop_counting() { send("disable\n"); }

 private:
  // Sends a command and waits for perf to acknowledge it, so the counters
  // have switched by the time this returns.
  void send(std::string_view command);

  int commands_ = -1;
  int acknowledgements_ = -1;
};

}  // namespace bbtree_bench
