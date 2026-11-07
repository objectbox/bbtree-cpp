// The time from a program's start to its map's first answer when the map's
// entries are on disk, as after a restart: `std::map` reads them back and
// builds itself again; a `bbtree_map_file` maps its file and looks the key up
// where it lies, reading only the pages the lookup reaches (the Rust crate's
// `examples/first_query.rs`, on mimalloc as it is).
//
//   bbtree_mmap_first_query [entries] [directory]
//
// Random `u64` keys and values, 10M by default; the files go to `directory`,
// the system's temporary one by default, which must be on a disk, not in
// memory (tmpfs). Each restart is a process of its own (this program, run
// again), run once with the file out of the page cache, as after a reboot,
// and once with the file in it. Prints CSV, a row a restart: the time to load
// the file (`std::map` reads it, the `bbtree_map_file` maps it), to build the
// map (`std::map`'s only), and to the first answer, each from the restart's
// start.
#include <mimalloc-new-delete.h>
#include <bbtree_mmap/bbtree_map_file.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bbtree_mmap_examples {

using std::chrono::nanoseconds;

// The map's entries in key order: distinct random `u64` keys, each with a
// value made from it.
class random_entries {
 public:
  explicit random_entries(std::size_t count) : entries_(count) {
    for (std::uint64_t index = 0; index < count; ++index) {
      const std::uint64_t key = mixed(index);
      entries_[index] = {key, value_of(key)};
    }
    std::sort(entries_.begin(), entries_.end());
  }

  static std::uint64_t value_of(std::uint64_t key) noexcept { return std::rotl(key, 17); }

  const std::vector<std::pair<std::uint64_t, std::uint64_t>>& entries() const noexcept { return entries_; }

 private:
  // SplitMix64's output for its `index`th state: random-looking, and
  // distinct for distinct indexes.
  static std::uint64_t mixed(std::uint64_t index) noexcept {
    std::uint64_t z = (index + 1) * 0x9e37'79b9'7f4a'7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58'476d'1ce4'e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d0'49bb'1331'11ebULL;
    return z ^ (z >> 31);
  }

  std::vector<std::pair<std::uint64_t, std::uint64_t>> entries_;
};

// When a restart's steps ended, from its start, and the value it found.
class first_answer {
 public:
  first_answer(nanoseconds loaded, nanoseconds built, nanoseconds answered,
               std::optional<std::uint64_t> value)
      : loaded_(loaded), built_(built), answered_(answered), value_(value) {}

  // The line a restart's process prints, for its parent to parse.
  std::string line() const {
    std::ostringstream line;
    line << loaded_.count() << ' ' << built_.count() << ' ' << answered_.count() << ' ';
    if (value_) {
      line << *value_;
    } else {
      line << "none";
    }
    return line.str();
  }

  static first_answer parse(const std::string& line) {
    std::istringstream fields(line);
    long long loaded = 0;
    long long built = 0;
    long long answered = 0;
    std::string value;
    if (!(fields >> loaded >> built >> answered >> value)) {
      throw std::runtime_error("first_query: a restart printed \"" + line + "\"");
    }
    std::optional<std::uint64_t> found;
    if (value != "none") {
      found = std::stoull(value);
    }
    return first_answer(nanoseconds(loaded), nanoseconds(built), nanoseconds(answered), found);
  }

  // Microseconds to load, to build after loading, and to the answer.
  std::string csv() const {
    std::array<char, 96> text{};
    std::snprintf(text.data(), text.size(), "%.1f,%.1f,%.1f", micros(loaded_), micros(built_ - loaded_),
                  micros(answered_));
    return text.data();
  }

  std::optional<std::uint64_t> value() const noexcept { return value_; }

 private:
  static double micros(nanoseconds time) noexcept { return static_cast<double>(time.count()) / 1e3; }

  nanoseconds loaded_;
  nanoseconds built_;
  nanoseconds answered_;
  std::optional<std::uint64_t> value_;
};

// Where a restart finds its file: out of the page cache, read from disk, or
// in it.
class page_cache {
 public:
  enum class state : std::uint8_t { cold, warm };

  explicit page_cache(state cache_state) noexcept : state_(cache_state) {}

  void prepare(const std::filesystem::path& path) const {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      throw std::system_error(errno, std::generic_category(), "first_query: open");
    }
    int error = 0;
    if (state_ == state::cold) {
      error = ::fsync(fd) == 0 ? ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) : errno;
    } else {
      std::array<char, 1 << 16> buffer{};
      ssize_t got = 0;
      while ((got = ::read(fd, buffer.data(), buffer.size())) > 0) {
      }
      error = got < 0 ? errno : 0;
    }
    ::close(fd);
    if (error != 0) {
      throw std::system_error(error, std::generic_category(), "first_query: prepare the page cache");
    }
  }

  std::string_view name() const noexcept { return state_ == state::cold ? "cold" : "warm"; }

 private:
  state state_;
};

// How a restarted program gets its map back, each from a file of its own.
class restart {
 public:
  enum class kind : std::uint8_t {
    // `std::map`, from a file of its entries in key order, each key and value
    // 8 little-endian bytes: read whole, then built from them, in order.
    std_map,
    // A `bbtree_map_file`, mapped: opening it reads its first page, the lookup
    // the pages it reaches.
    bbtree_map_file,
  };
  static constexpr std::array all{kind::std_map, kind::bbtree_map_file};
  // The argument that makes the program one restart, in a process of its own.
  static constexpr std::string_view child_argument = "--restart";

  explicit restart(kind restart_kind) noexcept : kind_(restart_kind) {}

  static restart named(std::string_view name) {
    for (const kind restart_kind : all) {
      if (restart(restart_kind).name() == name) {
        return restart(restart_kind);
      }
    }
    throw std::invalid_argument("first_query: no restart named " + std::string(name));
  }

  std::string_view name() const noexcept { return kind_ == kind::std_map ? "std-map" : "bbtree-mmap"; }

  // Writes the file this restart gets `entries` back from, into `directory`;
  // returns its path.
  std::filesystem::path write(const std::filesystem::path& directory, const random_entries& entries) const {
    const auto path =
        directory / ("bbtree-first-query-" + std::string(name()) + "-" + std::to_string(::getpid()));
    if (kind_ == kind::std_map) {
      std::vector<std::byte> bytes;
      bytes.reserve(entries.entries().size() * 16);
      for (const auto& [key, value] : entries.entries()) {
        append_little_endian(bytes, key);
        append_little_endian(bytes, value);
      }
      bbtree_mmap::mapped_file::write(path, bytes);
    } else {
      const bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map(entries.entries().begin(),
                                                                         entries.entries().end());
      bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::create(path, map);
    }
    return path;
  }

  // Gets the map back from the file at `path` and looks `key` up, timed from
  // the start: what a restarted program does before its first answer.
  first_answer answer_first(const std::filesystem::path& path, std::uint64_t key) const {
    const auto started = std::chrono::steady_clock::now();
    const auto since_start = [&] {
      return std::chrono::duration_cast<nanoseconds>(std::chrono::steady_clock::now() - started);
    };
    if (kind_ == kind::std_map) {
      const bbtree_mmap::mapped_file bytes = bbtree_mmap::mapped_file::read(path, alignof(std::uint64_t));
      const nanoseconds loaded = since_start();
      std::map<std::uint64_t, std::uint64_t> map;
      const std::span<const std::byte> file = bytes.bytes();
      for (std::size_t at = 0; at + 16 <= file.size(); at += 16) {
        map.emplace_hint(map.end(), little_endian(file.subspan(at, 8)),
                         little_endian(file.subspan(at + 8, 8)));
      }
      const nanoseconds built = since_start();
      const auto found = map.find(key);
      const std::optional<std::uint64_t> value =
          found != map.end() ? std::optional(found->second) : std::nullopt;
      return first_answer(loaded, built, since_start(), value);
    }
    // Unchecked, as the Rust crate's `open` maps: a checked map reads every page.
    const auto map = bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::mmap_unchecked(path);
    const nanoseconds loaded = since_start();
    const std::optional<std::uint64_t> value = map.get(key);
    return first_answer(loaded, loaded, since_start(), value);
  }

  // Runs `answer_first` in a process of its own, this program run again as a
  // restarted one, after leaving the file at `path` in or out of the page cache.
  first_answer answer_first_in_new_process(const std::filesystem::path& path, std::uint64_t key,
                                           page_cache cache) const {
    cache.prepare(path);
    std::array<int, 2> pipe_ends{};
    if (::pipe(pipe_ends.data()) != 0) {
      throw std::system_error(errno, std::generic_category(), "first_query: pipe");
    }
    std::string child_flag(child_argument);
    std::string restart_name(name());
    std::string path_text = path.string();
    std::string key_text = std::to_string(key);
    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child < 0) {
      throw std::system_error(errno, std::generic_category(), "first_query: fork");
    }
    if (child == 0) {
      ::dup2(pipe_ends[1], STDOUT_FILENO);
      ::close(pipe_ends[0]);
      ::close(pipe_ends[1]);
      std::array<char*, 6> arguments{child_flag.data(), child_flag.data(), restart_name.data(),
                                     path_text.data(),  key_text.data(),   nullptr};
      ::execv("/proc/self/exe", arguments.data());
      ::_exit(127);
    }
    ::close(pipe_ends[1]);
    std::string output;
    std::array<char, 256> buffer{};
    ssize_t got = 0;
    while ((got = ::read(pipe_ends[0], buffer.data(), buffer.size())) > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(got));
    }
    ::close(pipe_ends[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      throw std::runtime_error("first_query: the " + restart_name + " restart failed");
    }
    return first_answer::parse(output);
  }

 private:
  static void append_little_endian(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      bytes.push_back(static_cast<std::byte>(value >> shift));
    }
  }

  static std::uint64_t little_endian(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      value |= std::to_integer<std::uint64_t>(bytes[index]) << (8 * index);
    }
    return value;
  }

  kind kind_;
};

class first_query {
 public:
  first_query(std::size_t count, std::filesystem::path directory)
      : count_(count), directory_(std::move(directory)) {}

  void run() const {
    const random_entries entries(count_);
    // A key from the middle, the same for every restart.
    const std::uint64_t key = entries.entries()[count_ / 2].first;
    std::vector<std::pair<restart, std::filesystem::path>> files;
    for (const restart::kind kind : restart::all) {
      files.emplace_back(restart(kind), restart(kind).write(directory_, entries));
    }
    std::printf("map,page_cache,entries,file_mb,load_us,build_us,first_answer_us\n");
    for (const auto cache_state : {page_cache::state::cold, page_cache::state::warm}) {
      const page_cache cache(cache_state);
      for (const auto& [restarted, path] : files) {
        const first_answer answer = restarted.answer_first_in_new_process(path, key, cache);
        if (answer.value() != random_entries::value_of(key)) {
          throw std::runtime_error("first_query: " + std::string(restarted.name()) + " answered wrong");
        }
        const double file_mb = static_cast<double>(std::filesystem::file_size(path)) / 1e6;
        std::printf("%.*s,%.*s,%zu,%.1f,%s\n", static_cast<int>(restarted.name().size()),
                    restarted.name().data(), static_cast<int>(cache.name().size()), cache.name().data(),
                    count_, file_mb, answer.csv().c_str());
      }
    }
    for (const auto& file : files) {
      std::filesystem::remove(file.second);
    }
  }

  // Runs one restart, as the child `answer_first_in_new_process` starts:
  // `--restart <name> <path> <key>`.
  static void run_restart(std::string_view name, const std::filesystem::path& path, std::uint64_t key) {
    const first_answer answer = restart::named(name).answer_first(path, key);
    std::printf("%s\n", answer.line().c_str());
  }

 private:
  std::size_t count_;
  std::filesystem::path directory_;
};

}  // namespace bbtree_mmap_examples

int main(int argc, char** argv) {
  using bbtree_mmap_examples::first_query;
  using bbtree_mmap_examples::restart;
  if (argc == 5 && argv[1] == restart::child_argument) {
    first_query::run_restart(argv[2], argv[3], std::strtoull(argv[4], nullptr, 10));
    return 0;
  }
  const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
  std::filesystem::path directory = argc > 2 ? argv[2] : std::filesystem::temp_directory_path();
  first_query(count, std::move(directory)).run();
}
