#include "perf_counters.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <system_error>
#include <utility>

#include "named.hpp"
#include "text_file.hpp"
#include "text_lines.hpp"

namespace bbtree_bench {

std::span<const std::string_view> perf_events::perf_names(perf_event event) noexcept {
  static constexpr std::array<std::string_view, 1> cycles{"cycles:u"};
  static constexpr std::array<std::string_view, 1> instructions{"instructions:u"};
  static constexpr std::array<std::string_view, 1> branches{"branches:u"};
  static constexpr std::array<std::string_view, 1> branch_misses{"branch-misses:u"};
  static constexpr std::array<std::string_view, 1> l1d_load_misses{"L1-dcache-load-misses:u"};
  static constexpr std::array<std::string_view, 2> lines_from_memory{
      // AMD Zen 4 and later: L1d fills from DRAM or I/O, demand and prefetch.
      "ls_any_fills_from_sys.dram_io_all:u",
      // Intel: the core's requests that miss L3: loads, stores and
      // prefetches.
      "longest_lat_cache.miss:u",
  };
  switch (event) {
    case perf_event::cycles:
      return cycles;
    case perf_event::instructions:
      return instructions;
    case perf_event::branches:
      return branches;
    case perf_event::branch_misses:
      return branch_misses;
    case perf_event::l1d_load_misses:
      return l1d_load_misses;
    case perf_event::lines_from_memory:
      return lines_from_memory;
  }
  return {};
}

std::string_view perf_events::column(perf_event event) noexcept {
  switch (event) {
    case perf_event::cycles:
      return "cycles_per_op";
    case perf_event::instructions:
      return "instructions_per_op";
    case perf_event::branches:
      return "branches_per_op";
    case perf_event::branch_misses:
      return "branch_misses_per_op";
    case perf_event::l1d_load_misses:
      return "l1d_load_misses_per_op";
    case perf_event::lines_from_memory:
      return "lines_from_memory_per_op";
  }
  return "";
}

std::string_view perf_events::title(perf_event event) noexcept {
  switch (event) {
    case perf_event::cycles:
      return "cycles";
    case perf_event::instructions:
      return "instructions";
    case perf_event::branches:
      return "branches";
    case perf_event::branch_misses:
      return "branch misses";
    case perf_event::l1d_load_misses:
      return "L1d misses";
    case perf_event::lines_from_memory:
      return "lines from memory";
  }
  return "";
}

perf_event_names perf_event_names::for_this_cpu() {
  names_type names{};
  for (const perf_event event : perf_events::all) {
    std::vector<std::string> failures;
    for (const std::string_view candidate : perf_events::perf_names(event)) {
      try {
        only(event, candidate).count_trial();
        names[static_cast<std::size_t>(event)] = candidate;
        break;
      } catch (const bench_error& failure) {
        failures.emplace_back(failure.what());
      }
    }
    if (names[static_cast<std::size_t>(event)].empty()) {
      std::string reasons;
      for (const std::string& failure : failures) {
        reasons += reasons.empty() ? failure : "; " + failure;
      }
      throw bench_error(std::format("perf can't count {} here: {}", perf_events::title(event), reasons));
    }
  }
  const perf_event_names event_names(names);
  try {
    event_names.count_trial();
  } catch (const bench_error& failure) {
    throw bench_error(
        std::format("{}. The group needs a hardware counter per event at once: another perf session may hold "
                    "some, or, on "
                    "AMD, the NMI watchdog (`sysctl kernel.nmi_watchdog=0` frees it)",
                    failure.what()));
  }
  return event_names;
}

perf_event_names perf_event_names::only(perf_event event, std::string_view name) noexcept {
  names_type names{};
  names[static_cast<std::size_t>(event)] = name;
  return perf_event_names(names);
}

void perf_event_names::count_trial() const {
  child_command command = perf_stat();
  command.arg("--").arg("true");
  const child_output result = command.output();
  // `perf stat -x` reports on stderr.
  const std::string& report = result.standard_error;
  if (!result.status.success()) {
    throw bench_error(std::format("perf stat failed: {}", text_lines::trimmed(report)));
  }
  (void)perf_counts::parse(report, *this);
}

child_command perf_event_names::perf_stat() const {
  std::string group;
  for (const std::string_view name : names_) {
    if (!name.empty()) {
      group += group.empty() ? std::string(name) : "," + std::string(name);
    }
  }
  child_command command("perf");
  command.arg("stat").arg("-x").arg(",").arg("--no-inherit").arg("-e").arg("{" + group + "}");
  return command;
}

perf_counts perf_counts::parse(std::string_view text, const perf_event_names& names) {
  perf_counts counts;
  for (std::size_t index = 0; index < perf_events::all.size(); ++index) {
    const std::string_view name = names.name(index);
    if (name.empty()) {
      continue;
    }
    std::optional<std::vector<std::string_view>> found;
    for (const std::string_view line : text_lines::of(text)) {
      std::vector<std::string_view> fields = text_lines::split(line, ',');
      if (fields.size() > 2 && fields[2] == name) {
        found = std::move(fields);
        break;
      }
    }
    if (!found) {
      throw bench_error(std::format("perf reported no count of {}", name));
    }
    const std::vector<std::string_view>& fields = *found;
    if (fields.size() < 5) {
      std::string joined;
      for (const std::string_view field : fields) {
        joined += joined.empty() ? std::string(field) : "," + std::string(field);
      }
      throw bench_error(std::format("perf reported `{}` for {}", joined, name));
    }
    const std::optional<double> value = text_lines::parse_double(fields[0]);
    if (!value) {
      throw bench_error(std::format("perf couldn't count {}: {}", name, fields[0]));
    }
    const std::optional<double> running = text_lines::parse_double(fields[4]);
    if (!running || *running < 100.0) {
      throw bench_error(std::format("perf counted {} for {}% of the time only", name, fields[4]));
    }
    counts.counts_[index] = *value;
  }
  return counts;
}

perf_stat perf_stat::prepare(std::filesystem::path directory) {
  const perf_event_names names = perf_event_names::for_this_cpu();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    throw bench_error(std::format("creating {}: {}", directory.string(), error.message()));
  }
  perf_stat stat(std::move(directory), names);
  for (const std::filesystem::path& fifo : {stat.control_path(), stat.ack_path()}) {
    child_command mkfifo("mkfifo");
    mkfifo.arg(fifo.string());
    if (!mkfifo.status().success()) {
      throw bench_error(std::format("mkfifo {} failed", fifo.string()));
    }
  }
  return stat;
}

perf_stat::perf_stat(perf_stat&& other) noexcept
    : directory_(std::move(other.directory_)), event_names_(other.event_names_) {
  other.directory_.clear();
}

perf_stat::~perf_stat() {
  // Best effort: a leftover scratch directory is harmless.
  if (!directory_.empty()) {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }
}

std::string perf_stat::control_argument() const {
  return std::format("{},{}", control_path().string(), ack_path().string());
}

child_command perf_stat::command(const std::filesystem::path& program,
                                 const std::vector<std::string>& arguments) const {
  child_command command = event_names_.perf_stat();
  command.arg("--delay=-1").arg("-o").arg(counts_path().string());
  command.arg("--control=fifo:" + control_argument()).arg("--").arg(program.string()).args(arguments);
  return command;
}

perf_counts perf_stat::read_counts() const {
  const std::filesystem::path path = counts_path();
  const std::optional<std::string> text = text_file::read(path);
  if (!text) {
    throw bench_error(std::format("reading {} failed", path.string()));
  }
  return perf_counts::parse(*text, event_names_);
}

perf_control::perf_control(std::string_view argument) {
  const std::size_t comma = argument.find(',');
  if (comma == std::string_view::npos) {
    throw bench_error(std::format("--perf-control takes CONTROL,ACK, not `{}`", argument));
  }
  const std::string control(argument.substr(0, comma));
  const std::string ack(argument.substr(comma + 1));
  commands_ = ::open(control.c_str(), O_WRONLY | O_CLOEXEC);
  if (commands_ < 0) {
    throw bench_error(std::format("opening perf's control FIFOs: {}", std::strerror(errno)));
  }
  acknowledgements_ = ::open(ack.c_str(), O_RDONLY | O_CLOEXEC);
  if (acknowledgements_ < 0) {
    ::close(commands_);
    throw bench_error(std::format("opening perf's control FIFOs: {}", std::strerror(errno)));
  }
}

perf_control::perf_control(perf_control&& other) noexcept
    : commands_(std::exchange(other.commands_, -1)),
      acknowledgements_(std::exchange(other.acknowledgements_, -1)) {}

perf_control::~perf_control() {
  if (commands_ >= 0) {
    ::close(commands_);
  }
  if (acknowledgements_ >= 0) {
    ::close(acknowledgements_);
  }
}

void perf_control::send(std::string_view command) {
  bench_error::unless(
      ::write(commands_, command.data(), command.size()) == static_cast<ssize_t>(command.size()),
      "perf's control FIFO accepts commands");
  std::array<char, 16> reply{};
  const ssize_t length = ::read(acknowledgements_, reply.data(), reply.size());
  bench_error::unless(length >= 3 && std::string_view(reply.data(), 3) == "ack",
                      "perf acknowledges commands");
}

}  // namespace bbtree_bench
