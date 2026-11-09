// File maps (D58): a map written, then read where it lies, matches the map;
// another layout, misaligned or truncated bytes are refused; corrupted bytes
// give answers or a refusal, never undefined behavior (run under ASan).
#include <bbtree/file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

enum class color : std::uint8_t { red, green };

// Without padding, and still not its bytes: a file's byte 2 in `live` is no
// `bool`, and `name` points into the process that wrote it.
struct flagged {
  std::uint32_t id;
  bool live;
  char reserved[3];
  const char* name;
};

// A struct that is its bytes, which only its author can say.
struct version {
  std::uint16_t release;
  std::uint16_t patch;
};

}  // namespace

template <>
struct bbtree::is_plain_bytes<version> : std::true_type {};

TEST_CASE("plain bytes are the types whose values are their bytes", "[file]") {
  STATIC_REQUIRE(bbtree::plain_bytes<std::uint64_t>);
  STATIC_REQUIRE(bbtree::plain_bytes<std::int8_t>);
  STATIC_REQUIRE(bbtree::plain_bytes<char>);
  STATIC_REQUIRE(bbtree::plain_bytes<bbtree::uint128>);
  STATIC_REQUIRE(bbtree::plain_bytes<bbtree::int128>);
  STATIC_REQUIRE(bbtree::plain_bytes<std::byte>);
  STATIC_REQUIRE(bbtree::plain_bytes<float>);
  STATIC_REQUIRE(bbtree::plain_bytes<double>);
  STATIC_REQUIRE(bbtree::plain_bytes<std::array<std::uint8_t, 32>>);
  STATIC_REQUIRE(bbtree::plain_bytes<std::array<std::array<std::uint16_t, 2>, 3>>);
  STATIC_REQUIRE(bbtree::plain_bytes<std::monostate>);
  STATIC_REQUIRE(bbtree::plain_bytes<version>);
  STATIC_REQUIRE(bbtree::plain_bytes<std::array<version, 4>>);

  // A byte that is no value of the type, when a file holds it.
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<bool>);
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<std::array<bool, 8>>);
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<color>);
  // Bytes that mean something in the process that wrote them alone.
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<const char*>);
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<std::string_view>);
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<std::span<const std::uint8_t>>);
  // Padding: bytes no write initializes.
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<long double>);
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<std::pair<std::uint8_t, std::uint32_t>>);
  // No padding says nothing of the members.
  STATIC_REQUIRE(std::has_unique_object_representations_v<flagged>);
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<flagged>);
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<std::string>);
  STATIC_REQUIRE_FALSE(bbtree::plain_bytes<std::array<std::uint8_t, 0>>);
}

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

// The same entries make the same file map, however the map got them: it is
// compact, and holds no trace of the map's or the writer's addresses. (The
// Rust crate's `file_maps_depend_on_the_entries_alone`; the shared map has no
// writer here.)
TEST_CASE("file maps depend on the entries alone", "[file]") {
  using writer = bbtree::file_writer<std::uint64_t, std::uint64_t>;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> in_order;
  for (std::uint64_t key = 0; key < 5000; ++key) {
    entries.emplace_back(key, ~key);
    in_order.insert({key, ~key});
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> shuffled_entries = entries;
  std::shuffle(shuffled_entries.begin(), shuffled_entries.end(), std::mt19937_64(5));
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> shuffled;
  for (const auto& [key, value] : shuffled_entries) {
    shuffled.insert({key, value});
    // Entries that come and go, so that the map's nodes held others before.
    shuffled.insert({key + 5000, value});
  }
  for (std::uint64_t key = 5000; key < 10'000; ++key) {
    shuffled.erase(key);
  }
  // The writers live side by side: none builds its tree where another's was.
  const writer ascending = writer::from_ascending(entries.begin(), entries.end());
  const writer of_in_order(in_order);
  const writer of_shuffled(shuffled);
  const bbtree::file_buf file_bytes = ascending.to_buf();
  const auto same_bytes = [&](const bbtree::file_buf& other) {
    return std::equal(other.bytes().begin(), other.bytes().end(), file_bytes.bytes().begin(),
                      file_bytes.bytes().end());
  };
  CHECK(same_bytes(of_in_order.to_buf()));
  CHECK(same_bytes(of_shuffled.to_buf()));
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

TEST_CASE("a file map's pages start at multiples of their page size", "[file]") {
  // Nodes in memory are aligned to a cache line; in a file map a 1 KiB page
  // never crosses one of the system's 4 KiB pages.
  using map_type = bbtree::file_map<std::uint64_t, std::uint64_t>;
  CHECK(map_type::alignment() == 1024);
  bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t> map;
  for (std::uint64_t key = 0; key < 5000; ++key) {
    map.insert({key, key});
  }
  const bbtree::file_buf bytes = bbtree::file_writer<std::uint64_t, std::uint64_t>(map).to_buf();
  const bbtree::detail::file_header header = bbtree::detail::file_header::read(bytes.bytes());
  CHECK(header.root == 1024);
  CHECK(header.byte_len % 1024 == 0);
  // A reader 64 bytes off its alignment is refused, though its nodes would
  // be aligned as nodes in memory are.
  bbtree::file_buf shifted(bytes.bytes().size() + 64, map_type::alignment());
  std::copy(bytes.bytes().begin(), bytes.bytes().end(), shifted.bytes().begin() + 64);
  CHECK_THROWS_AS(map_type::open(shifted.bytes().subspan(64)), bbtree::file_error);
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
