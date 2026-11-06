// Map files: typed maps written, mapped and read, plain and var-length keys
// and values, after the Rust crate's `bbtree-mmap/tests`.
#include <bbtree_mmap/bbtree_map_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

namespace {

std::filesystem::path temp_path(const char* name) {
  return std::filesystem::temp_directory_path() /
         (std::string("bbtree-mmap-") + name + "-" + std::to_string(::getpid()));
}

}  // namespace

TEST_CASE("plain keys and values round-trip through a mapped file", "[mmap]") {
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map;
  for (std::uint64_t key = 0; key < 50'000; ++key) {
    map.insert({key * 7, key});
  }
  const auto path = temp_path("plain");
  bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::create(path, map);
  for (const bool mapped : {true, false}) {
    const auto file = mapped ? bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::mmap(path)
                             : bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::read(path);
    REQUIRE(file.size() == map.size());
    for (std::uint64_t key = 0; key < 350'010; key += 13) {
      REQUIRE(file.get(key).has_value() == map.contains(key));
      if (const auto value = file.get(key)) {
        REQUIRE(*value == key / 7);
      }
    }
  }
  std::filesystem::remove(path);
}

TEST_CASE("string keys and values keep their bytes after their leaves", "[mmap]") {
  bbtree::unsync::bbtree_map<std::string, std::string> map;
  std::mt19937_64 rng(3);
  for (int i = 0; i < 20'000; ++i) {
    std::string key = "/var/data/" + std::to_string(rng() % 100) + "/" + std::to_string(i);
    std::string value(rng() % 3 == 0 ? 3000 : rng() % 80, static_cast<char>('a' + i % 26));
    map.insert_or_assign(std::move(key), std::move(value));
  }
  const auto path = temp_path("strings");
  bbtree_mmap::bbtree_map_file<std::string, std::string>::create(path, map);
  const auto file =
      bbtree_mmap::bbtree_map_file<std::string, std::string>::mmap(path, bbtree_mmap::os_pages::huge);
  REQUIRE(file.size() == map.size());
  for (const auto& [key, value] : map) {
    const auto found = file.get(std::string_view(key));
    REQUIRE(found.has_value());
    REQUIRE(*found == value);
  }
  CHECK_FALSE(file.get(std::string_view("/var/data/none")).has_value());
  CHECK_FALSE(file.contains(std::string_view("")));
  std::size_t visited = 0;
  auto expected = map.begin();
  file.for_each([&](std::string_view key, std::string_view value) {
    REQUIRE(key == expected->first);
    REQUIRE(value == expected->second);
    ++expected;
    ++visited;
  });
  CHECK(visited == map.size());
  std::filesystem::remove(path);
}

TEST_CASE("u64 keys with byte-vector values", "[mmap]") {
  bbtree::unsync::bbtree_map<std::uint64_t, std::vector<std::uint8_t>> map;
  for (std::uint64_t key = 0; key < 5000; ++key) {
    map.insert({key, std::vector<std::uint8_t>(key % 50, static_cast<std::uint8_t>(key))});
  }
  const auto path = temp_path("vectors");
  bbtree_mmap::bbtree_map_file<std::uint64_t, std::vector<std::uint8_t>>::create(path, map);
  const auto file = bbtree_mmap::bbtree_map_file<std::uint64_t, std::vector<std::uint8_t>>::read(path);
  for (std::uint64_t key = 0; key < 5000; ++key) {
    const auto found = file.get(key);
    REQUIRE(found.has_value());
    REQUIRE(found->size() == key % 50);
  }
  std::filesystem::remove(path);
}
