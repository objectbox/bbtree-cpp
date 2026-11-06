#include "child_process.hpp"

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>

#include "named.hpp"

extern char** environ;

namespace bbtree_bench {

namespace {

// A pipe's two ends, closed when it goes.
class pipe_ends {
 public:
  pipe_ends() {
    if (::pipe(ends_.data()) != 0) {
      throw bench_error(std::format("creating a pipe: {}", std::strerror(errno)));
    }
  }
  pipe_ends(const pipe_ends&) = delete;
  pipe_ends& operator=(const pipe_ends&) = delete;
  ~pipe_ends() {
    close_read();
    close_write();
  }

  int read_end() const noexcept { return ends_[0]; }
  int write_end() const noexcept { return ends_[1]; }

  void close_read() noexcept { close_end(ends_[0]); }
  void close_write() noexcept { close_end(ends_[1]); }

 private:
  static void close_end(int& end) noexcept {
    if (end >= 0) {
      ::close(end);
      end = -1;
    }
  }

  std::array<int, 2> ends_{-1, -1};
};

// Spawn actions, destroyed when they go.
class spawn_actions {
 public:
  spawn_actions() noexcept { ::posix_spawn_file_actions_init(&actions_); }
  spawn_actions(const spawn_actions&) = delete;
  spawn_actions& operator=(const spawn_actions&) = delete;
  ~spawn_actions() { ::posix_spawn_file_actions_destroy(&actions_); }

  void redirect(int from, int to) noexcept { ::posix_spawn_file_actions_adddup2(&actions_, from, to); }
  void close(int descriptor) noexcept { ::posix_spawn_file_actions_addclose(&actions_, descriptor); }

  const posix_spawn_file_actions_t* get() const noexcept { return &actions_; }

 private:
  posix_spawn_file_actions_t actions_;
};

}  // namespace

bool child_status::success() const noexcept {
  return WIFEXITED(wait_status_) && WEXITSTATUS(wait_status_) == 0;
}

std::string child_status::text() const {
  if (WIFEXITED(wait_status_)) {
    return std::format("exit status: {}", WEXITSTATUS(wait_status_));
  }
  if (WIFSIGNALED(wait_status_)) {
    return std::format("signal: {} ({})", WTERMSIG(wait_status_), ::strsignal(WTERMSIG(wait_status_)));
  }
  return std::format("wait status {}", wait_status_);
}

std::vector<char*> child_command::argv() const {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(program_.c_str()));
  for (const std::string& argument : arguments_) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  return argv;
}

child_output child_command::output() const {
  pipe_ends standard_output;
  pipe_ends standard_error;
  spawn_actions actions;
  actions.redirect(standard_output.write_end(), STDOUT_FILENO);
  actions.redirect(standard_error.write_end(), STDERR_FILENO);
  actions.close(standard_output.read_end());
  actions.close(standard_error.read_end());
  pid_t child = 0;
  const std::vector<char*> arguments = argv();
  const int spawned =
      ::posix_spawnp(&child, program_.c_str(), actions.get(), nullptr, arguments.data(), environ);
  if (spawned != 0) {
    throw bench_error(std::format("starting {}: {}", program_, std::strerror(spawned)));
  }
  standard_output.close_write();
  standard_error.close_write();

  std::string captured_output;
  std::string captured_error;
  std::array<pollfd, 2> streams{pollfd{standard_output.read_end(), POLLIN, 0},
                                pollfd{standard_error.read_end(), POLLIN, 0}};
  std::array<std::string*, 2> captures{&captured_output, &captured_error};
  std::array<char, 65536> buffer{};
  std::size_t open_streams = streams.size();
  while (open_streams > 0) {
    if (::poll(streams.data(), streams.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw bench_error(std::format("waiting for {}: {}", program_, std::strerror(errno)));
    }
    for (std::size_t stream = 0; stream < streams.size(); ++stream) {
      if (streams[stream].fd < 0 || streams[stream].revents == 0) {
        continue;
      }
      const ssize_t read = ::read(streams[stream].fd, buffer.data(), buffer.size());
      if (read > 0) {
        captures[stream]->append(buffer.data(), static_cast<std::size_t>(read));
      } else if (read == 0 || errno != EINTR) {
        streams[stream].fd = -1;
        --open_streams;
      }
    }
  }
  int wait_status = 0;
  while (::waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {
  }
  return child_output{child_status(wait_status), std::move(captured_output), std::move(captured_error)};
}

child_status child_command::status() const {
  pid_t child = 0;
  const std::vector<char*> arguments = argv();
  const int spawned = ::posix_spawnp(&child, program_.c_str(), nullptr, nullptr, arguments.data(), environ);
  if (spawned != 0) {
    throw bench_error(std::format("starting {}: {}", program_, std::strerror(spawned)));
  }
  int wait_status = 0;
  while (::waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {
  }
  return child_status(wait_status);
}

std::optional<std::string> child_command::trimmed_output_if_success() const {
  try {
    const child_output result = output();
    if (!result.status.success()) {
      return std::nullopt;
    }
    const std::string& text = result.standard_output;
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return std::string();
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
  } catch (const bench_error&) {
    return std::nullopt;
  }
}

}  // namespace bbtree_bench
