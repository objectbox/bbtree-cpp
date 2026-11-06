// File maps (D58): a map written, then read where it lies, matches the map;
// another layout, misaligned or truncated bytes are refused; corrupted bytes
// give answers or a refusal, never undefined behavior (run under ASan).
#include <bbtree/file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

TEST_CASE("a file map reads like the map it was written from", "[file]") {
  for (const std::size_t count : {0u, 1u, 30u, 1000u, 100'000u}) {
    bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map;
    std::mt19937_64 rng(count);
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint64_t key = rng() % (count * 3 + 1);
      map.insert_or_assign(key, key * 2);
    }
    const bbtree::file_buf bytes = bbtree::file_writer<std::uint64_t, std::uint64_t>(map).to_buf();
    const auto read = bbtree::file_map<std::uint64_t, std::uint64_t>::open(bytes.bytes());
    REQUIRE(read.size() == map.size());
    for (std::uint64_t key = 0; key < count * 3 + 2; key += 7) {
      const std::uint64_t* value = read.get(key);
      REQUIRE((value != nullptr) == map.contains(key));
      if (value != nullptr) {
        REQUIRE(*value == key * 2);
      }
    }
    REQUIRE(std::equal(read.begin(), read.end(), map.begin(), map.end(),
                       [](auto a, auto b) { return a.first == b.first && a.second == b.second; }));
    if (count > 0) {
      const auto from = read.lower_bound(count);
      const auto expected = map.lower_bound(count);
      REQUIRE((from == read.end()) == (expected == map.end()));
    }
  }
}

TEST_CASE("file maps of byte-array keys and a set-like value", "[file]") {
  std::vector<std::pair<std::array<std::uint8_t, 16>, std::uint32_t>> entries;
  for (std::uint32_t i = 0; i < 20'000; ++i) {
    std::array<std::uint8_t, 16> key{};
    key[0] = 0x42;
    key[12] = static_cast<std::uint8_t>(i >> 24);
    key[13] = static_cast<std::uint8_t>(i >> 16);
    key[14] = static_cast<std::uint8_t>(i >> 8);
    key[15] = static_cast<std::uint8_t>(i);
    entries.emplace_back(key, i);
  }
  using writer = bbtree::file_writer<std::array<std::uint8_t, 16>, std::uint32_t>;
  const bbtree::file_buf bytes = writer::from_ascending(entries.begin(), entries.end()).to_buf();
  const auto read = bbtree::file_map<std::array<std::uint8_t, 16>, std::uint32_t>::open(bytes.bytes());
  for (const auto& [key, value] : entries) {
    REQUIRE(read.get(key) != nullptr);
    REQUIRE(*read.get(key) == value);
  }
  CHECK_THROWS_AS(writer::from_ascending(entries.rbegin(), entries.rend()), std::invalid_argument);
}

TEST_CASE("a file map of another layout, misaligned or truncated is refused", "[file]") {
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map;
  for (std::uint64_t key = 0; key < 5000; ++key) {
    map.insert({key, key});
  }
  const bbtree::file_buf bytes = bbtree::file_writer<std::uint64_t, std::uint64_t>(map).to_buf();
  CHECK_THROWS_AS((bbtree::file_map<std::uint64_t, std::uint32_t>::open(bytes.bytes())), bbtree::file_error);
  CHECK_THROWS_AS((bbtree::file_map<std::uint64_t, std::uint64_t>::open(bytes.bytes().subspan(8))),
                  bbtree::file_error);
  CHECK_THROWS_AS(
      (bbtree::file_map<std::uint64_t, std::uint64_t>::open(bytes.bytes().first(bytes.bytes().size() / 2))),
      bbtree::file_error);
  CHECK_THROWS_AS((bbtree::file_map<std::uint64_t, std::uint64_t>::open(bytes.bytes().first(64))),
                  bbtree::file_error);
}

TEST_CASE("corrupted file maps give answers or a refusal, never a crash", "[file]") {
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map;
  for (std::uint64_t key = 0; key < 20'000; ++key) {
    map.insert({key * 3, key});
  }
  const bbtree::file_buf original = bbtree::file_writer<std::uint64_t, std::uint64_t>(map).to_buf();
  std::mt19937_64 rng(13);
  int refused = 0;
  for (int round = 0; round < 300; ++round) {
    bbtree::file_buf bytes(original.bytes().size(),
                           bbtree::file_map<std::uint64_t, std::uint64_t>::alignment());
    std::copy(original.bytes().begin(), original.bytes().end(), bytes.bytes().begin());
    for (int flip = 0; flip < 8; ++flip) {
      const std::size_t at = 128 + rng() % (bytes.bytes().size() - 128);
      bytes.bytes()[at] = static_cast<std::byte>(rng());
    }
    try {
      const auto read = bbtree::file_map<std::uint64_t, std::uint64_t>::open(bytes.bytes());
      std::size_t visited = 0;
      for (auto it = read.begin(); it != read.end() && visited < 100'000; ++it) {
        ++visited;
      }
      for (std::uint64_t key = 0; key < 60'000; key += 101) {
        (void)read.get(key);
      }
    } catch (const bbtree::file_error&) {
      ++refused;
    }
  }
  CHECK(refused >= 0);
}
