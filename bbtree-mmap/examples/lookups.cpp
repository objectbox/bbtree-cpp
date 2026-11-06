// Times lookups in a map file mapped from disk, against the map it was written
// from: `bbtree_mmap_lookups [entries]` (the Rust crate's `examples/lookups.rs`).
#include <bbtree_mmap/bbtree_map_file.hpp>

#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

int main(int argc, char** argv) {
  const std::uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
  const auto path = std::filesystem::temp_directory_path() / ("bbtree-lookups-" + std::to_string(::getpid()));
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map;
  std::mt19937_64 rng(1);
  std::vector<std::uint64_t> keys(count);
  for (auto& key : keys) {
    key = rng();
    map.insert({key, key});
  }
  bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::create(path, map);
  const auto file = bbtree_mmap::bbtree_map_file<std::uint64_t, std::uint64_t>::mmap(path);
  std::shuffle(keys.begin(), keys.end(), rng);
  auto time = [&](auto&& look_up) {
    std::uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const std::uint64_t key : keys) {
      sum += look_up(key);
    }
    const auto ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    return std::pair(ns / static_cast<double>(count), sum);
  };
  const auto [mapped_ns, mapped_sum] = time([&](std::uint64_t key) { return *file.get(key); });
  const auto [map_ns, map_sum] = time([&](std::uint64_t key) { return *map.get(key); });
  std::printf("%llu entries: map file %.1f ns a hit, map %.1f ns (%s)\n",
              static_cast<unsigned long long>(count), mapped_ns, map_ns,
              mapped_sum == map_sum ? "same values" : "VALUES DIFFER");
  std::filesystem::remove(path);
  return mapped_sum == map_sum ? 0 : 1;
}
