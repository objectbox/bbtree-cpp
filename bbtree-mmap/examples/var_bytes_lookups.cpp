// Lookups of var-length keys and values, kept in heap pages after the leaves,
// against plain ones kept in the leaves, and against a map (the Rust crate's
// `examples/var_bytes_lookups.rs`, on mimalloc as it is).
//
//   bbtree_mmap_var_bytes_lookups [entries]   (10M by default)
//
// Two key sets, each with random lookups that hit and that miss:
//
// - random `u64` keys and values: a map, a `bbtree_map_file<u64, u64>`, which
//   keeps them in their slots, and a file map of their 8 big-endian bytes,
//   var-length, in heap pages after the leaves, looked up by those bytes: what
//   keeping bytes apart costs;
// - random text keys of 8 to 40 bytes, after a shared 5-byte prefix, with
//   `u64` values: a map of `std::string`s and a `bbtree_map_file<std::string, u64>`.
//
// The file maps are read into memory. Their files go to the system's
// temporary directory.
#include <mimalloc-new-delete.h>
#include <bbtree_mmap/bbtree_map_file.hpp>

#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "timed_lookups.hpp"

namespace bbtree_mmap_examples {

class var_bytes_lookups {
 public:
  explicit var_bytes_lookups(std::size_t count) : count_(count) {}

  void run() {
    u64_lookups();
    text_lookups();
  }

 private:
  using bytes = std::vector<std::uint8_t>;
  using big_endian_u64 = std::array<std::uint8_t, 8>;

  // Random `u64` keys and values: a map, a file map, which keeps them in their
  // slots, and a file map of var-length ones.
  void u64_lookups() {
    const auto map_and_lookups = timed_lookups<std::uint64_t>::of_u64_map(count_, numbers_);
    const auto& map = map_and_lookups.first;
    const auto& lookups = map_and_lookups.second;
    const auto path = temp_path("bbtree-var-bytes-lookups-");
    bbtree::unsync::bbtree_map<bytes, bytes> var_entries;
    map.for_each([&](std::uint64_t key, std::uint64_t value) {
      const big_endian_u64 key_bytes = to_big_endian(key);
      const big_endian_u64 value_bytes = to_big_endian(value);
      var_entries.insert(
          {bytes(key_bytes.begin(), key_bytes.end()), bytes(value_bytes.begin(), value_bytes.end())});
    });
    bbtree_mmap::bbtree_map_file<bytes, bytes>::create(path, var_entries);
    var_entries.clear();
    const auto var_map = bbtree_mmap::bbtree_map_file<bytes, bytes>::read(path);
    const auto var_len = std::filesystem::file_size(path);
    bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::create(path, map);
    const auto disk = bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::read(path);
    const auto disk_len = std::filesystem::file_size(path);
    std::filesystem::remove(path);
    std::printf("%zu u64 entries: in slots, %.2f bytes an entry; var-length, %.2f\n", count_,
                static_cast<double>(disk_len) / static_cast<double>(count_),
                static_cast<double>(var_len) / static_cast<double>(count_));
    lookups.report("u64: map", [&](std::uint64_t key) { return found(map.get(key)); });
    lookups.report("u64: bbtree_map_file<u64, u64>", [&](std::uint64_t key) { return disk.get(key); });
    lookups.report("u64: file map, var-length", [&](std::uint64_t key) -> std::optional<std::uint64_t> {
      const big_endian_u64 key_bytes = to_big_endian(key);
      const auto value = var_map.get(std::span<const std::uint8_t>(key_bytes));
      if (!value) {
        return std::nullopt;
      }
      return from_big_endian(*value);
    });
  }

  // Random text keys with `u64` values: a map of `std::string`s, and a file map.
  void text_lookups() {
    std::vector<std::string> texts(count_);
    for (auto& text : texts) {
      text = random_text();
    }
    std::sort(texts.begin(), texts.end());
    texts.erase(std::unique(texts.begin(), texts.end()), texts.end());
    bbtree::unsync::bbtree_map<std::string, std::uint64_t> map;
    for (const auto& text : texts) {
      map.insert({text, text.size()});
    }
    std::vector<std::string> hits(count_);
    for (auto& hit : hits) {
      hit = texts[numbers_.below(texts.size())];
    }
    // Absent: a byte no key has, at the end.
    std::vector<std::string> misses(count_);
    for (auto& miss : misses) {
      miss = texts[numbers_.below(texts.size())] + "!";
    }
    texts = {};
    const timed_lookups<std::string> lookups(std::move(hits), std::move(misses));
    const auto path = temp_path("bbtree-text-lookups-");
    bbtree_mmap::bbtree_map_file<std::string, std::uint64_t>::create(path, map);
    const auto disk = bbtree_mmap::bbtree_map_file<std::string, std::uint64_t>::read(path);
    const auto file_len = std::filesystem::file_size(path);
    std::filesystem::remove(path);
    std::printf("%zu text entries: a bbtree_map_file of %.2f bytes an entry\n", map.size(),
                static_cast<double>(file_len) / static_cast<double>(map.size()));
    lookups.report("text: map of std::string", [&](const std::string& key) { return found(map.get(key)); });
    lookups.report("text: bbtree_map_file<std::string, u64>",
                   [&](const std::string& key) { return disk.get(key); });
  }

  // Text of 8 to 40 letters and digits after `user/`.
  std::string random_text() {
    static constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    const std::size_t len = 8 + numbers_.below(33);
    std::string text = "user/";
    for (std::size_t index = 0; index < len; ++index) {
      text += alphabet[numbers_.below(alphabet.size())];
    }
    return text;
  }

  static std::filesystem::path temp_path(const std::string& prefix) {
    return std::filesystem::temp_directory_path() / (prefix + std::to_string(::getpid()));
  }

  static std::optional<std::uint64_t> found(const std::uint64_t* value) {
    return value != nullptr ? std::optional(*value) : std::nullopt;
  }

  static big_endian_u64 to_big_endian(std::uint64_t value) noexcept {
    big_endian_u64 big_endian{};
    for (std::size_t index = 0; index < big_endian.size(); ++index) {
      big_endian[index] = static_cast<std::uint8_t>(value >> (56 - 8 * index));
    }
    return big_endian;
  }

  static std::uint64_t from_big_endian(std::span<const std::uint8_t> big_endian) {
    if (big_endian.size() != sizeof(std::uint64_t)) {
      throw std::runtime_error("var_bytes_lookups: a value not of 8 bytes");
    }
    std::uint64_t value = 0;
    for (const std::uint8_t byte : big_endian) {
      value = value << 8 | byte;
    }
    return value;
  }

  std::size_t count_;
  seeded_numbers numbers_;
};

}  // namespace bbtree_mmap_examples

int main(int argc, char** argv) {
  const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10'000'000;
  bbtree_mmap_examples::var_bytes_lookups(count).run();
}
