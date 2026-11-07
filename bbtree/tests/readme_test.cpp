// The README's examples, compiled and run, so that they stay true.
#include <bbtree/compact_string.hpp>
#include <bbtree/map.hpp>
#include <bbtree/unsync/map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

TEST_CASE("the README's one-thread example", "[readme]") {
  bbtree::unsync::bbtree_map<std::uint64_t, std::string> map;
  map.insert({1, "one"});
  map[2] = "two";
  std::string joined;
  for (const auto& [key, value] : map) {
    joined += std::to_string(key) + value;
  }
  CHECK(joined == "1one2two");
  map.for_each([](const auto&, auto& value) { value += '!'; });
  CHECK(map[1] == "one!");
  auto upper = map.split_off(2);
  CHECK(upper.size() == 1);
  std::array<std::uint64_t, 64> keys{};
  keys[0] = 1;
  const auto values = map.get_many(keys);
  CHECK(*values[0] == "one!");
}

TEST_CASE("the README's threads example", "[readme]") {
  bbtree::bbtree_map<std::uint64_t, std::uint64_t> map;
  {
    std::jthread a([&] { map.insert(1, 10); });
    std::jthread b([&] { map.insert(2, 20); });
  }
  CHECK(map.get_copied(1) == 10);
  CHECK(map.read(2, [](const auto&, const auto& value) { return value + 1; }) == 21);
  map.update(1, [](const auto&, auto& value) { value *= 2; });
  CHECK(map.get_copied(1) == 20);
  int visited = 0;
  map.scan([&](const auto&, const auto&) {
    ++visited;
    return true;
  });
  CHECK(visited == 2);
}

TEST_CASE("the README's compact_string example", "[readme]") {
  bbtree::unsync::bbtree_map<bbtree::compact_string, int, std::less<>> map;
  map[std::string("key")] = 1;
  CHECK(map.find(std::string_view("key"))->second == 1);
  std::string_view first = map.begin()->first;
  CHECK(first == "key");
}
