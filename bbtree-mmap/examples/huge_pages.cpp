// A `bbtree_map_file` mapped from disk, as after a restart, in each size of
// OS pages (`os_pages`): the mapping reads the file in the pages it asks for.
// Base pages read a few KiB for each page a lookup first reaches, and miss the
// TLB more once in memory; huge ones read 2 MiB, and then look up at a map's
// speed (the Rust crate's `examples/huge_pages.rs`, on mimalloc as it is).
//
//   bbtree_mmap_huge_pages [entries] [directory]
//
// Random `u64` keys and values, 10M by default; the file goes to `directory`,
// the system's temporary one by default, which must be on a disk, not in
// memory (tmpfs).
#include <mimalloc-new-delete.h>
#include <bbtree_mmap/bbtree_map_file.hpp>

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include "timed_lookups.hpp"

namespace bbtree_mmap_examples {

using u64_map_file = bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>;

// A map file's file, which mappings read from disk; removed when destroyed.
class cold_file {
 public:
  // Writes `map` to the file at `path`.
  cold_file(std::filesystem::path path, const bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t>& map)
      : path_(std::move(path)) {
    u64_map_file::create(path_, map);
  }
  cold_file(const cold_file&) = delete;
  cold_file& operator=(const cold_file&) = delete;
  ~cold_file() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  // The map, mapped in `pages` from disk: the file is dropped from the page
  // cache first, and every page read in before this returns. Mapped
  // unchecked, as the Rust crate's `open` maps.
  u64_map_file map_from_disk(bbtree_mmap::os_pages pages) const {
    drop_from_page_cache();
    u64_map_file mapped = u64_map_file::mmap_unchecked(path_, pages);
    std::uint64_t sum = 0;
    mapped.for_each([&](std::uint64_t, std::uint64_t value) { sum += value; });
    asm volatile("" : : "r,m"(sum) : "memory");
    return mapped;
  }

 private:
  void drop_from_page_cache() const {
    const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      throw std::system_error(errno, std::generic_category(), "huge_pages: open");
    }
    const bool synced = ::fsync(fd) == 0;
    const int advice_error = synced ? ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) : errno;
    ::close(fd);
    if (!synced || advice_error != 0) {
      throw std::system_error(advice_error, std::generic_category(), "huge_pages: drop from the page cache");
    }
  }

  std::filesystem::path path_;
};

// How much of the process's memory the kernel maps with 2 MiB entries: of
// files, as the mapped map file, and anonymous, as the map.
class huge_page_mappings {
 public:
  huge_page_mappings() {
    std::ifstream rollup("/proc/self/smaps_rollup");
    std::string line;
    while (std::getline(rollup, line)) {
      if (line.starts_with("FilePmdMapped") || line.starts_with("AnonHugePages")) {
        std::istringstream words(line);
        std::string word;
        std::string joined;
        while (words >> word) {
          joined += joined.empty() ? word : " " + word;
        }
        text_ += text_.empty() ? joined : ", " + joined;
      }
    }
  }

  const std::string& text() const noexcept { return text_; }

 private:
  std::string text_;
};

class huge_pages {
 public:
  huge_pages(std::size_t count, std::filesystem::path directory)
      : count_(count), directory_(std::move(directory)) {}

  void run() {
    seeded_numbers numbers;
    const auto map_and_lookups = timed_lookups<std::uint64_t>::of_u64_map(count_, numbers);
    const auto& map = map_and_lookups.first;
    const auto& lookups = map_and_lookups.second;
    const cold_file file(directory_ / ("bbtree-huge-pages-" + std::to_string(::getpid())), map);

    lookups.report("map", [&](std::uint64_t key) -> std::optional<std::uint64_t> {
      const std::uint64_t* value = map.get(key);
      return value != nullptr ? std::optional(*value) : std::nullopt;
    });
    for (const auto& [pages, pages_name] :
         {std::pair(bbtree_mmap::os_pages::base, "base"), std::pair(bbtree_mmap::os_pages::huge, "huge")}) {
      const u64_map_file mapped = file.map_from_disk(pages);
      lookups.report("bbtree_map_file, mapped, " + std::string(pages_name) + " pages",
                     [&](std::uint64_t key) { return mapped.get(key); });
      std::printf("  (%s)\n", huge_page_mappings().text().c_str());
    }
  }

 private:
  std::size_t count_;
  std::filesystem::path directory_;
};

}  // namespace bbtree_mmap_examples

int main(int argc, char** argv) {
  const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
  std::filesystem::path directory = argc > 2 ? argv[2] : std::filesystem::temp_directory_path();
  bbtree_mmap_examples::huge_pages(count, std::move(directory)).run();
}
