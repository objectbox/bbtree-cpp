// Child processes: the Rust bench's `std::process::Command`, which the suite
// runner starts each cell with, perf counts them under, `fetch` downloads
// with, and the report header asks git and date through.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bbtree_bench {

// How a child ended.
class child_status {
 public:
  explicit child_status(int wait_status) noexcept : wait_status_(wait_status) {}

  bool success() const noexcept;

  // As Rust prints an `ExitStatus`: `exit status: 1`, or `signal: 11`.
  std::string text() const;

 private:
  int wait_status_;
};

// What a child wrote, and how it ended.
struct child_output {
  child_status status;
  std::string standard_output;
  std::string standard_error;
};

// A program and its arguments, found on `PATH` unless the program has a
// slash.
class child_command {
 public:
  explicit child_command(std::string program) : program_(std::move(program)) {}

  child_command& arg(std::string argument) {
    arguments_.push_back(std::move(argument));
    return *this;
  }

  child_command& args(const std::vector<std::string>& arguments) {
    arguments_.insert(arguments_.end(), arguments.begin(), arguments.end());
    return *this;
  }

  const std::string& program() const noexcept { return program_; }

  // Runs the child with its standard output and error captured; throws if it
  // can't be started.
  child_output output() const;

  // Runs the child with this process's standard streams; throws if it can't
  // be started.
  child_status status() const;

  // The trimmed standard output of a child that succeeded; none if it
  // failed or couldn't start.
  std::optional<std::string> trimmed_output_if_success() const;

 private:
  std::vector<char*> argv() const;

  std::string program_;
  std::vector<std::string> arguments_;
};

}  // namespace bbtree_bench
