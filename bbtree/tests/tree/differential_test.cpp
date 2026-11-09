// Random inserts, lookups and removals on the tree, against std::map, with the
// structural checks after every operation (the Rust crate's differential
// tests and fuzz targets, D5).
#include <bbtree/detail/tree/tree.hpp>

#include "../support/test_types.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <type_traits>

namespace {

using bbtree::detail::tree_types;

// The tree with its allocator and length: what a map holds.
template <class K, class V, class S>
class tree_under_test {
 public:
  using types = tree_types<K, V, std::less<K>, S>;

  ~tree_under_test() { tree_.destroy(pages_); }

  bool insert_or_assign(K key, V value) {
    auto result = tree_.template insert<bbtree::detail::existing_value::assigned>(
        std::move(key), [&] { return value; }, pages_, length_);
    return result.inserted;
  }
  const V* find(const K& key) const {
    const auto found = tree_.find(key);
    return found ? found->value() : nullptr;
  }
  std::optional<std::pair<K, V>> remove(const K& key) { return tree_.remove(key, pages_, length_); }
  std::optional<std::pair<K, V>> pop_first() { return tree_.pop_first(pages_, length_); }
  std::optional<std::pair<K, V>> pop_last() { return tree_.pop_last(pages_, length_); }
  void check() const { tree_.check_invariants(length_); }
  // Whether the next key past the last would append without a descent.
  bool remembers_last_leaf() const { return static_cast<bool>(tree_.remembered_rightmost_leaf()); }
  std::size_t size() const { return length_; }
  std::size_t depth() const { return tree_.has_root() ? tree_.leaf_depth() : 0; }

 private:
  bbtree::global_allocator allocator_;
  bbtree::detail::page_source<bbtree::global_allocator> pages_{allocator_};
  bbtree::detail::tree<types> tree_;
  std::size_t length_ = 0;
};

template <class K>
struct key_maker;

template <>
struct key_maker<std::uint64_t> {
  static std::uint64_t make(std::uint64_t n) { return n * 2654435761u % 100'003; }
};
template <>
struct key_maker<std::int64_t> {
  static std::int64_t make(std::uint64_t n) { return static_cast<std::int64_t>(n % 50'000) - 25'000; }
};
template <>
struct key_maker<std::string> {
  // Paths that share long prefixes, to exercise prefix stripping (D26, D50).
  static std::string make(std::uint64_t n) {
    std::string key = "/usr/share/data/";
    key += std::to_string(n % 37);
    key += "/item-";
    key += std::to_string(n % 4001);
    return key;
  }
};
template <>
struct key_maker<std::array<std::uint8_t, 16>> {
  static std::array<std::uint8_t, 16> make(std::uint64_t n) {
    std::array<std::uint8_t, 16> key{};
    const std::uint64_t value = n % 20'000;
    key[0] = 0x7a;
    for (std::size_t i = 0; i < 8; ++i) {
      key[8 + i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
    }
    return key;
  }
};

template <>
struct key_maker<bbtree_test::user_string> {
  static bbtree_test::user_string make(std::uint64_t n) { return {key_maker<std::string>::make(n)}; }
};

// A key with no inline keys: a struct ordered by its operator<=>.
struct plain_key {
  std::uint32_t high;
  std::uint32_t low;
  friend auto operator<=>(const plain_key&, const plain_key&) = default;
};
template <>
struct key_maker<plain_key> {
  static plain_key make(std::uint64_t n) {
    return {static_cast<std::uint32_t>(n % 97), static_cast<std::uint32_t>(n % 3001)};
  }
};

// Makes the test's values: plain integers, strings, or tracked values that a
// ledger counts.
template <class V>
struct value_maker {
  static V make(std::uint64_t n, bbtree_test::drop_ledger&) { return static_cast<V>(n); }
};
template <>
struct value_maker<std::string> {
  static std::string make(std::uint64_t n, bbtree_test::drop_ledger&) {
    return "value number " + std::to_string(n) + " with a heap buffer";
  }
};
template <>
struct value_maker<bbtree_test::tracked> {
  static bbtree_test::tracked make(std::uint64_t n, bbtree_test::drop_ledger& ledger) { return {ledger, n}; }
};

template <class K, class V, class S>
void run_differential_with(std::uint64_t seed, int operations, std::uint64_t key_space,
                           bbtree_test::drop_ledger& ledger) {
  tree_under_test<K, V, S> tree;
  std::map<K, V> model;
  std::mt19937_64 rng(seed);

  for (int step = 0; step < operations; ++step) {
    const std::uint64_t n = rng() % key_space;
    const K key = key_maker<K>::make(n);
    const unsigned op = static_cast<unsigned>(rng() % 10);
    if (op < 5) {
      const V value = value_maker<V>::make(rng(), ledger);
      const bool inserted = tree.insert_or_assign(key, value);
      const bool model_inserted = model.insert_or_assign(key, value).second;
      REQUIRE(inserted == model_inserted);
    } else if (op < 8) {
      auto removed = tree.remove(key);
      auto it = model.find(key);
      REQUIRE(removed.has_value() == (it != model.end()));
      if (removed) {
        REQUIRE(removed->first == it->first);
        REQUIRE(removed->second == it->second);
        model.erase(it);
      }
    } else if (op < 9) {
      auto popped = rng() % 2 == 0 ? tree.pop_first() : tree.pop_last();
      REQUIRE(popped.has_value() == !model.empty());
      if (popped) {
        auto it = model.find(popped->first);
        REQUIRE(it != model.end());
        REQUIRE((it == model.begin() || std::next(it) == model.end()));
        REQUIRE(it->second == popped->second);
        model.erase(it);
      }
    } else {
      const V* found = tree.find(key);
      auto it = model.find(key);
      REQUIRE((found != nullptr) == (it != model.end()));
      if (found) {
        REQUIRE(*found == it->second);
      }
    }
    REQUIRE(tree.size() == model.size());
    if (step % 64 == 0 || operations < 5000) {
      tree.check();
    }
  }
  tree.check();
  for (const auto& [key, value] : model) {
    const V* found = tree.find(key);
    REQUIRE(found != nullptr);
    REQUIRE(*found == value);
  }
}

template <class K, class V, class S>
void run_differential(std::uint64_t seed, int operations, std::uint64_t key_space) {
  bbtree_test::drop_ledger ledger;
  {
    run_differential_with<K, V, S>(seed, operations, key_space, ledger);
  }
  REQUIRE(ledger.alive() == 0);
}
}  // namespace

TEMPLATE_TEST_CASE("the tree matches std::map", "[tree][differential]", std::uint64_t, std::int64_t,
                   std::string, (std::array<std::uint8_t, 16>), plain_key) {
  using K = TestType;
  SECTION("inline keys where the key has them") {
    for (std::uint64_t seed = 0; seed < 3; ++seed) {
      run_differential<K, std::uint64_t, bbtree::auto_inline_keys>(seed, 40'000, 6000);
    }
  }
  SECTION("plain keys") {
    for (std::uint64_t seed = 0; seed < 3; ++seed) {
      run_differential<K, std::uint64_t, bbtree::plain_keys>(seed, 40'000, 6000);
    }
  }
}

TEMPLATE_TEST_CASE("the tree moves values that are not trivially relocatable", "[tree][differential]",
                   std::string, bbtree_test::tracked) {
  run_differential<std::string, TestType, bbtree::auto_inline_keys>(7, 20'000, 2000);
  run_differential<std::uint64_t, TestType, bbtree::auto_inline_keys>(8, 20'000, 4000);
  run_differential<plain_key, TestType, bbtree::plain_keys>(9, 20'000, 4000);
}

TEST_CASE("keys with their own inline_key specialization", "[tree][differential]") {
  using types = bbtree::detail::tree_types<bbtree_test::user_string, std::uint64_t,
                                           std::less<bbtree_test::user_string>, bbtree::auto_inline_keys>;
  STATIC_CHECK(types::leaf_plan.hash_tags);
  STATIC_CHECK(types::internal_plan.rehydrates_windows);
  STATIC_CHECK(*types::internal_plan.width == bbtree::inline_width::four);
  for (std::uint64_t seed = 0; seed < 3; ++seed) {
    run_differential<bbtree_test::user_string, std::uint64_t, bbtree::auto_inline_keys>(seed, 30'000, 6000);
  }
}

TEST_CASE("ascending inserts append and grow the tree", "[tree]") {
  tree_under_test<std::uint64_t, std::uint64_t, bbtree::auto_inline_keys> tree;
  for (std::uint64_t key = 0; key < 200'000; ++key) {
    REQUIRE(tree.insert_or_assign(key, key + 1));
  }
  tree.check();
  CHECK(tree.depth() >= 2);
  for (std::uint64_t key = 0; key < 200'000; key += 997) {
    REQUIRE(tree.find(key) != nullptr);
    REQUIRE(*tree.find(key) == key + 1);
  }
  while (tree.pop_first()) {
  }
  tree.check();
  CHECK(tree.size() == 0);
}

TEST_CASE("inserts that fit and upserts keep the remembered last leaf", "[tree]") {
  tree_under_test<std::uint64_t, std::uint64_t, bbtree::auto_inline_keys> tree;
  std::uint64_t next = 0;
  for (; next < 1000; ++next) {
    REQUIRE(tree.insert_or_assign(next * 1000, next));
  }
  REQUIRE(tree.remembers_last_leaf());

  // An existing key, and a new one into a leaf with room: no node moves.
  CHECK_FALSE(tree.insert_or_assign(5000, 1));
  CHECK(tree.remembers_last_leaf());
  CHECK(tree.insert_or_assign(5001, 1));
  CHECK(tree.remembers_last_leaf());
  tree.check();

  // A leaf that splits elsewhere forgets it, until the last leaf splits.
  for (std::uint64_t key = 5002; tree.remembers_last_leaf(); ++key) {
    REQUIRE(key < 5999);
    REQUIRE(tree.insert_or_assign(key, 1));
    tree.check();
  }
  while (!tree.remembers_last_leaf()) {
    REQUIRE(tree.insert_or_assign(next * 1000, next));
    ++next;
    tree.check();
  }
}

TEST_CASE("a key that shares less keeps the remembered last leaf", "[tree]") {
  tree_under_test<std::string, std::uint64_t, bbtree::auto_inline_keys> tree;
  std::map<std::string, std::uint64_t> model;
  auto insert = [&](const std::string& key, std::uint64_t value) {
    REQUIRE(tree.insert_or_assign(key, value) == model.insert_or_assign(key, value).second);
    tree.check();
  };
  auto path = [](std::uint64_t n) {
    std::string digits = std::to_string(n);
    return "/usr/share/data/item-" + std::string(6 - digits.size(), '0') + digits;
  };
  std::uint64_t next = 0;
  for (; next < 500; ++next) {
    insert(path(next), next);
  }
  REQUIRE(tree.remembers_last_leaf());
  // Shares "/usr/" only: every border node's prefix moves back, the last
  // leaf's too, and appends go on into it.
  insert("/usr/lib", 1);
  CHECK(tree.remembers_last_leaf());
  for (; next < 1000; ++next) {
    insert(path(next), next);
  }
  // Shares nothing, and is past the last key.
  insert("zebra", 2);
  for (; next < 1500; ++next) {
    insert(path(next), next);
  }
  REQUIRE(tree.size() == model.size());
  for (const auto& [key, value] : model) {
    const std::uint64_t* found = tree.find(key);
    REQUIRE(found != nullptr);
    REQUIRE(*found == value);
  }
}

TEMPLATE_TEST_CASE("appends mixed with inserts, upserts and removals match std::map", "[tree][differential]",
                   std::uint64_t, std::string) {
  using K = TestType;
  auto key_of = [](std::uint64_t n) {
    if constexpr (std::is_same_v<K, std::string>) {
      std::string digits = std::to_string(n);
      return "key-" + std::string(12 - digits.size(), '0') + digits;
    } else {
      return n;
    }
  };
  tree_under_test<K, std::uint64_t, bbtree::auto_inline_keys> tree;
  std::map<K, std::uint64_t> model;
  std::mt19937_64 rng(11);
  std::uint64_t next = 0;
  for (std::uint64_t step = 0; step < 30'000; ++step) {
    const unsigned op = static_cast<unsigned>(rng() % 8);
    if (op < 3 || next == 0) {
      // Past the last key: an append.
      const K key = key_of(next * 4 + 2);
      ++next;
      REQUIRE(tree.insert_or_assign(key, step));
      model.insert_or_assign(key, step);
    } else if (op < 6) {
      // A key inside, new or not.
      const K key = key_of((rng() % next) * 4 + rng() % 3);
      REQUIRE(tree.insert_or_assign(key, step) == model.insert_or_assign(key, step).second);
    } else if (op < 7) {
      const K key = key_of((rng() % next) * 4 + 2);
      REQUIRE(tree.remove(key).has_value() == (model.erase(key) == 1));
    } else if (rng() % 64 == 0 && !model.empty()) {
      REQUIRE(tree.pop_last()->first == std::prev(model.end())->first);
      model.erase(std::prev(model.end()));
    }
    // The remembered leaf is the tree's last, with everything else.
    tree.check();
  }
  REQUIRE(tree.size() == model.size());
  for (const auto& [key, value] : model) {
    const std::uint64_t* found = tree.find(key);
    REQUIRE(found != nullptr);
    REQUIRE(*found == value);
  }
}
