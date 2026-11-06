// A store against std::map, on an allocator that counts its pages by kind
// and may refuse them past a limit; and threads sharing a store (the Rust
// crate's `tests/store.rs`).
#include <bbtree_store/store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

using byte_string = std::vector<std::uint8_t>;
using bbtree_store::bytes_of;
using bbtree_store::insert_result;

// Pages out, by kind: internal nodes, leaves, heap pages.
struct page_tally {
  std::atomic<long> internal{0};
  std::atomic<long> leaf{0};
  std::atomic<long> heap{0};
  // At most this many pages of every kind out at once; 0 for any.
  std::atomic<long> limit{0};

  long out() const noexcept { return internal.load() + leaf.load() + heap.load(); }
};

// Hands out pages of the global allocator, counting them by kind; a copy
// counts into the same tally, as copies of one allocator.
class counting_pages {
 public:
  explicit counting_pages(page_tally& tally) noexcept : tally_(&tally) {}

  void* allocate_zeroed(bbtree::page_layout layout) const noexcept {
    return bbtree::global_allocator{}.allocate_zeroed(layout);
  }
  void deallocate(void* page, bbtree::page_layout layout) const noexcept {
    bbtree::global_allocator{}.deallocate(page, layout);
  }
  void* allocate_internal(bbtree::page_layout layout) const noexcept {
    return take(tally_->internal, layout);
  }
  void deallocate_internal(void* page, bbtree::page_layout layout) const noexcept {
    give(tally_->internal, page, layout);
  }
  void* allocate_leaf(bbtree::page_layout layout) const noexcept { return take(tally_->leaf, layout); }
  void deallocate_leaf(void* page, bbtree::page_layout layout) const noexcept {
    give(tally_->leaf, page, layout);
  }
  void* allocate_heap(bbtree::page_layout layout) const noexcept { return take(tally_->heap, layout); }
  void deallocate_heap(void* page, bbtree::page_layout layout) const noexcept {
    give(tally_->heap, page, layout);
  }

 private:
  void* take(std::atomic<long>& count, bbtree::page_layout layout) const noexcept {
    const long limit = tally_->limit.load();
    if (limit != 0 && tally_->out() >= limit) {
      return nullptr;
    }
    ++count;
    return bbtree::global_allocator{}.allocate_zeroed(layout);
  }

  // Takes a page back, scribbled over first, so that a record read after its
  // page went reads no key or value it held.
  static void give(std::atomic<long>& count, void* page, bbtree::page_layout layout) noexcept {
    --count;
    std::memset(page, 0xA5, layout.size);
    bbtree::global_allocator{}.deallocate(page, layout);
  }

  page_tally* tally_;
};

using counted_store = bbtree_store::bbtree_store<counting_pages>;

// The default node config's heap page size, the leaves'.
constexpr std::size_t page_size = 1024;

// The key numbered `n`: how many bytes `n` takes, then those bytes, then
// filler, from 2 to 40 bytes, some within an 8-byte inline key and some not.
// No two numbers share a key.
byte_string key(std::size_t n) {
  std::uint8_t big_endian[8];
  for (std::size_t byte = 0; byte < 8; ++byte) {
    big_endian[byte] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(n) >> (8 * (7 - byte)));
  }
  std::size_t skipped = 0;
  while (skipped < 7 && big_endian[skipped] == 0) {
    ++skipped;
  }
  byte_string made{static_cast<std::uint8_t>(8 - skipped)};
  made.insert(made.end(), big_endian + skipped, big_endian + 8);
  made.resize(made.size() + n % 32, 'k');
  return made;
}

byte_string value(std::size_t n, std::size_t version, std::size_t len) {
  byte_string made(len);
  for (std::size_t i = 0; i < len; ++i) {
    made[i] = static_cast<std::uint8_t>(n + version * 7 + i);
  }
  return made;
}

// A deterministic xorshift generator.
class xorshift {
 public:
  explicit xorshift(std::uint64_t seed) noexcept : state_(seed) {}
  std::size_t below(std::size_t bound) noexcept {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return static_cast<std::size_t>(state_ % bound);
  }

 private:
  std::uint64_t state_;
};

std::vector<std::pair<byte_string, byte_string>> scanned(const counted_store& store,
                                                         const bbtree_store::key_range& range) {
  std::vector<std::pair<byte_string, byte_string>> seen;
  store.scan(range, [&](std::span<const std::uint8_t> k, std::span<const std::uint8_t> v) {
    seen.emplace_back(byte_string(k.begin(), k.end()), byte_string(v.begin(), v.end()));
    return true;
  });
  return seen;
}

using model_map = std::map<byte_string, byte_string>;

// Scans both ways, and copies from both ends meeting, of a random range,
// against `model`.
void require_ranges_match(const counted_store& store, const model_map& model, xorshift& rng) {
  byte_string low = key(rng.below(3'000));
  byte_string high = key(rng.below(3'000));
  if (high < low) {
    std::swap(low, high);
  }
  const bbtree_store::key_range range{bbtree_store::key_bound::included(low),
                                      bbtree_store::key_bound::excluded(high)};
  std::vector<std::pair<byte_string, byte_string>> expected;
  for (auto at = model.lower_bound(low); at != model.end() && at->first < high; ++at) {
    expected.emplace_back(at->first, at->second);
  }
  REQUIRE(scanned(store, range) == expected);
  std::vector<std::pair<byte_string, byte_string>> backwards;
  store.scan_rev(range, [&](std::span<const std::uint8_t> k, std::span<const std::uint8_t> v) {
    backwards.emplace(backwards.begin(), byte_string(k.begin(), k.end()), byte_string(v.begin(), v.end()));
    return true;
  });
  REQUIRE(backwards == expected);
  auto copies = store.range(range);
  std::vector<std::pair<byte_string, byte_string>> front;
  std::vector<std::pair<byte_string, byte_string>> back;
  while (true) {
    const bool from_front = rng.below(2) == 0;
    std::optional<bbtree_store::entry_copy> entry = from_front ? copies.next() : copies.next_back();
    if (!entry) {
      break;
    }
    (from_front ? front : back).push_back(std::move(*entry));
  }
  front.insert(front.end(), back.rbegin(), back.rend());
  REQUIRE(front == expected);
}

// The length of the value numbered `n` in the threads' tests: some take
// overflow pages.
std::size_t threads_value_len(std::size_t n) {
  return n % 16 == 0 ? page_size + n % (3 * page_size) : n % 200;
}

}  // namespace

TEST_CASE("a store inserts, reads, replaces and removes", "[store]") {
  bbtree_store::bbtree_store<> store;
  CHECK_FALSE(store.get(bytes_of("apple")));
  CHECK(store.insert(bytes_of("apple"), bytes_of("red")) == insert_result::inserted);
  CHECK(store.insert(bytes_of("banana"), bytes_of("yellow")) == insert_result::inserted);
  CHECK(store.get(bytes_of("apple")) == byte_string{'r', 'e', 'd'});
  // A value no longer goes in place; a longer one in a new record.
  CHECK(store.insert(bytes_of("apple"), bytes_of("re")) == insert_result::replaced);
  CHECK(store.get(bytes_of("apple")) == byte_string{'r', 'e'});
  CHECK(store.insert(bytes_of("apple"), bytes_of("green and red")) == insert_result::replaced);
  const std::string_view green = "green and red";
  CHECK(store.get(bytes_of("apple")) == byte_string(green.begin(), green.end()));
  byte_string buf{'x'};
  CHECK(store.get_into(bytes_of("banana"), buf) == 6);
  CHECK(buf == byte_string{'x', 'y', 'e', 'l', 'l', 'o', 'w'});
  CHECK(store.contains(bytes_of("banana")));
  CHECK(store.remove(bytes_of("banana")));
  CHECK_FALSE(store.remove(bytes_of("banana")));
  CHECK_FALSE(store.contains(bytes_of("banana")));
  CHECK(store.insert(bytes_of(""), bytes_of("")) == insert_result::inserted);
  CHECK(store.get(bytes_of("")) == byte_string{});
}

// Keys longer than a quarter page are refused; values of any length go in,
// those too long for their record to fit in a page's room into overflow
// pages of their own, all full but for what is left over, which the record
// keeps if it fits, else a last page. Long values are replaced and removed,
// and the store drops holding some.
TEST_CASE("a store refuses long keys and takes values of any length", "[store]") {
  const std::size_t max_key = counted_store::max_key_len();
  {
    bbtree_store::bbtree_store<> store;
    CHECK(store.insert(byte_string(max_key + 1, 1), {}) == insert_result::key_too_long);
  }
  // A page's room, past its header of a pointer and two u32s (1,008 bytes
  // with 8-byte pointers), holds a record of nine bytes of lengths and
  // generation, the key, and a pointer-sized link before what the record
  // keeps of a long value; an overflow page's, past its link, the value's
  // bytes (1,016).
  constexpr std::size_t link = sizeof(void*);
  constexpr std::size_t room = page_size - link - 8;
  constexpr std::size_t overflow_room = page_size - link;
  const struct {
    std::size_t key_len;
    std::size_t value_len;
    long overflow_pages;
  } cases[] = {
      {20, 0, 0},
      {20, room - 9 - 20, 0},
      // Left over: all of it, which doesn't fit with the link.
      {20, room - 9 - 20 + 1, 1},
      {20, overflow_room, 1},
      {20, overflow_room + room - 9 - 20 - link, 1},
      {20, overflow_room + room - 9 - 20 - link + 1, 2},
      {20, 10 * overflow_room + 500, 10},
      {max_key, room - 9 - max_key, 0},
      {max_key, room - 9 - max_key + 1, 1},
      {max_key, 3 * overflow_room + room - 9 - max_key - link, 3},
  };
  for (const auto& each : cases) {
    page_tally tally;
    {
      counted_store store{counting_pages(tally)};
      const byte_string k(each.key_len, 1);
      const byte_string v = value(each.key_len, each.value_len, each.value_len);
      REQUIRE(store.insert(k, v) == insert_result::inserted);
      INFO("a " << each.key_len << "-byte key's " << each.value_len << "-byte value");
      CHECK(tally.heap.load() == 1 + each.overflow_pages);
      CHECK(store.get(k) == v);
      const auto all = scanned(store, bbtree_store::key_range::all());
      REQUIRE(all.size() == 1);
      CHECK(all[0].second == v);
    }
    CHECK(tally.out() == 0);
  }
  page_tally tally;
  {
    counted_store store{counting_pages(tally)};
    const std::size_t count = 64;
    for (std::size_t n = 0; n < count; ++n) {
      const std::size_t lens[] = {5'000, 10, 3 * page_size};
      REQUIRE(store.insert(key(n), value(n, 0, lens[n % 3])) == insert_result::inserted);
    }
    const std::size_t new_lens[] = {10, 4 * page_size, 0};
    for (std::size_t n = 0; n < count; ++n) {
      REQUIRE(store.insert(key(n), value(n, 1, new_lens[n % 3])) == insert_result::replaced);
      if (n % 5 == 0) {
        REQUIRE(store.remove(key(n)));
      }
    }
    for (std::size_t n = 0; n < count; ++n) {
      const std::optional<byte_string> expected =
          n % 5 != 0 ? std::optional(value(n, 1, new_lens[n % 3])) : std::nullopt;
      REQUIRE(store.get(key(n)) == expected);
    }
  }
  CHECK(tally.out() == 0);
}

// A long value whose overflow pages the allocator refuses partway goes in
// nowhere, with the pages it took given back, the store as it was.
TEST_CASE("a long value past the budget takes no page", "[store]") {
  page_tally tally;
  {
    counted_store store{counting_pages(tally)};
    REQUIRE(store.insert(bytes_of("apple"), bytes_of("red")) == insert_result::inserted);
    REQUIRE(store.insert(bytes_of("banana"), value(0, 0, 2 * page_size)) == insert_result::inserted);
    const long out = tally.out();
    tally.limit = out + 3;
    for (const char* k : {"apple", "banana", "cherry"}) {
      CHECK(store.insert(bytes_of(k), value(1, 1, 8 * page_size)) == insert_result::refused);
      CHECK(tally.out() == out);
    }
    CHECK(store.get(bytes_of("apple")) == byte_string{'r', 'e', 'd'});
    CHECK(store.get(bytes_of("banana")) == value(0, 0, 2 * page_size));
    CHECK_FALSE(store.get(bytes_of("cherry")));
  }
  CHECK(tally.out() == 0);
}

// Random inserts, replacements and removals, against std::map, with values
// from empty to a few heap pages: leaves split, merge and compact, their
// records move between leaves' chains, and long values' overflow pages move
// with their records.
TEST_CASE("a store matches std::map", "[store]") {
  page_tally tally;
  {
    counted_store store{counting_pages(tally)};
    model_map model;
    xorshift rng(0x9E3779B97F4A7C15);
    const std::size_t steps = 60'000;
    const std::size_t keys = 3'000;
    for (std::size_t step = 0; step < steps; ++step) {
      const std::size_t n = rng.below(keys);
      const byte_string entry_key = key(n);
      const std::size_t choice = rng.below(10);
      if (choice <= 5) {
        std::size_t len = 0;
        switch (rng.below(16)) {
          case 0:
          case 1:
            len = rng.below(page_size);
            break;
          case 2:
            len = rng.below(4 * page_size);
            break;
          case 3:
            len = 0;
            break;
          default:
            len = rng.below(64);
        }
        const byte_string entry_value = value(n, step, len);
        const insert_result result = store.insert(entry_key, entry_value);
        const bool replaced = !model.insert_or_assign(entry_key, entry_value).second;
        REQUIRE(result == (replaced ? insert_result::replaced : insert_result::inserted));
      } else if (choice <= 8) {
        REQUIRE(store.remove(entry_key) == (model.erase(entry_key) == 1));
      } else {
        const auto expected = model.find(entry_key);
        REQUIRE(store.get(entry_key) ==
                (expected == model.end() ? std::nullopt : std::optional(expected->second)));
      }
      if (step % (steps / 16) == 0) {
        require_ranges_match(store, model, rng);
      }
    }
    for (const auto& [k, v] : model) {
      REQUIRE(store.get(k) == v);
    }
    for (const auto& [k, v] : model) {
      REQUIRE(store.remove(k));
    }
  }
  CHECK(tally.out() == 0);
}

// Inserts past the allocator's limit fail, the store holding what it held.
// Removals take holes back without a page, by compacting, so that most new
// records of the removed ones' size fit again: not all, as the room a leaf's
// chain frees stays that leaf's.
TEST_CASE("a spent budget refuses inserts and changes nothing", "[store]") {
  page_tally tally;
  const long limit = 64;
  tally.limit = limit;
  {
    counted_store store{counting_pages(tally)};
    model_map model;
    std::size_t refused = 0;
    xorshift rng(0x2545F4914F6CDD1D);
    for (std::size_t step = 0; step < 3'000; ++step) {
      const std::size_t n = rng.below(2'000);
      const byte_string k = key(n);
      const byte_string v = value(n, step, 100);
      const insert_result result = store.insert(k, v);
      if (result == insert_result::refused) {
        ++refused;
      } else {
        const bool replaced = !model.insert_or_assign(k, v).second;
        REQUIRE(result == (replaced ? insert_result::replaced : insert_result::inserted));
      }
      REQUIRE(tally.out() <= limit);
    }
    CHECK(refused > 0);
    for (const auto& [k, v] : model) {
      REQUIRE(store.get(k) == v);
    }
    std::vector<byte_string> removed;
    std::size_t index = 0;
    for (const auto& entry : model) {
      if (index++ % 2 == 0) {
        removed.push_back(entry.first);
      }
    }
    for (const byte_string& k : removed) {
      REQUIRE(store.remove(k));
      model.erase(k);
    }
    std::size_t taken_back = 0;
    for (const byte_string& k : removed) {
      const byte_string v = value(0, 1, 100);
      if (store.insert(k, v) == insert_result::inserted) {
        model.emplace(k, v);
        ++taken_back;
      }
    }
    INFO(taken_back << " of " << removed.size() << " removed keys back in");
    CHECK(taken_back * 2 > removed.size());
    for (const auto& [k, v] : model) {
      REQUIRE(store.get(k) == v);
    }
  }
  CHECK(tally.out() == 0);
}

// Under churn, replacing and removing entries at random over a fixed key set,
// the holes go back: the heap stays under twice what the records take, as a
// leaf compacts once half its chain is holes, and keeps a head page with
// room.
TEST_CASE("holes go back under churn", "[store]") {
  page_tally tally;
  {
    counted_store store{counting_pages(tally)};
    model_map model;
    xorshift rng(0x123456789ABCDEF1);
    for (int round = 0; round < 8; ++round) {
      for (std::size_t step = 0; step < 20'000; ++step) {
        const std::size_t n = rng.below(4'000);
        const byte_string k = key(n);
        if (rng.below(4) == 0) {
          store.remove(k);
          model.erase(k);
        } else {
          const byte_string v = value(n, step, 16 + rng.below(96));
          REQUIRE(store.insert(k, v) != insert_result::refused);
          model.insert_or_assign(k, v);
        }
      }
      std::size_t entry_bytes = 0;
      for (const auto& [k, v] : model) {
        entry_bytes += 9 + k.size() + v.size();
      }
      const double ratio =
          static_cast<double>(tally.heap.load()) * page_size / static_cast<double>(entry_bytes);
      INFO("round " << round << ": " << ratio << " heap bytes a record byte");
      CHECK(ratio < 2.0);
    }
    for (const auto& [k, v] : model) {
      REQUIRE(store.get(k) == v);
    }
  }
  CHECK(tally.out() == 0);
}

// Threads insert, read and remove entries apart, some of whose values take
// overflow pages.
TEST_CASE("threads share a store", "[store][threads]") {
  constexpr std::size_t threads = 4;
  constexpr std::size_t per_thread = 5'000;
  page_tally tally;
  {
    counted_store store{counting_pages(tally)};
    std::atomic<int> wrong{0};
    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < threads; ++t) {
      workers.emplace_back([&, t] {
        for (std::size_t i = 0; i < per_thread; ++i) {
          const std::size_t n = i * threads + t;
          const byte_string k = key(n);
          const byte_string v = value(n, 0, threads_value_len(n));
          if (store.insert(k, v) != insert_result::inserted || store.get(k) != v) {
            wrong.fetch_add(1);
          }
          if (i % 3 == 0 && !store.remove(k)) {
            wrong.fetch_add(1);
          }
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    CHECK(wrong.load() == 0);
    for (std::size_t n = 0; n < threads * per_thread; ++n) {
      const bool present = (n / threads) % 3 != 0;
      REQUIRE(store.get(key(n)) ==
              (present ? std::optional(value(n, 0, threads_value_len(n))) : std::nullopt));
    }
  }
  CHECK(tally.out() == 0);
}

// Threads insert, replace and remove the same keys while others read and
// scan them: records move between leaves' chains, strays die in other
// leaves' pages, orphans free themselves, and every read sees a whole value
// of its key, in key order.
TEST_CASE("threads change the same keys while others read and scan", "[store][threads]") {
  constexpr std::size_t writers = 3;
  constexpr std::size_t keys = 2'000;
  // A value of key `n`: every byte n's, so that a torn or foreign value shows.
  const auto value_of = [](std::size_t n, std::size_t len) {
    return byte_string(len, static_cast<std::uint8_t>(n));
  };
  const auto is_value_of = [](std::size_t n, std::span<const std::uint8_t> v) {
    for (const std::uint8_t byte : v) {
      if (byte != static_cast<std::uint8_t>(n)) {
        return false;
      }
    }
    return true;
  };
  // The number of a key, from its bytes (`key`).
  const auto number_of = [](std::span<const std::uint8_t> k) {
    std::size_t n = 0;
    for (std::size_t byte = 1; byte <= k[0]; ++byte) {
      n = n << 8 | k[byte];
    }
    return n;
  };
  page_tally tally;
  {
    counted_store store{counting_pages(tally)};
    std::atomic<bool> stop{false};
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (std::size_t w = 0; w < writers; ++w) {
      threads.emplace_back([&, w] {
        xorshift rng(0x5DEECE66D + w);
        for (std::size_t step = 0; step < 30'000; ++step) {
          const std::size_t n = rng.below(keys);
          if (rng.below(3) == 0) {
            store.remove(key(n));
          } else {
            const std::size_t len = rng.below(8) == 0 ? rng.below(3 * page_size) : rng.below(120);
            if (store.insert(key(n), value_of(n, len)) == insert_result::refused) {
              wrong.fetch_add(1);
            }
          }
        }
      });
    }
    threads.emplace_back([&] {
      xorshift rng(42);
      while (!stop.load()) {
        const std::size_t n = rng.below(keys);
        const std::optional<byte_string> got = store.get(key(n));
        if (got && !is_value_of(n, *got)) {
          wrong.fetch_add(1);
        }
      }
    });
    threads.emplace_back([&] {
      while (!stop.load()) {
        std::optional<byte_string> previous;
        store.scan(bbtree_store::key_range::all(),
                   [&](std::span<const std::uint8_t> k, std::span<const std::uint8_t> v) {
                     const byte_string current(k.begin(), k.end());
                     if ((previous && !(*previous < current)) || !is_value_of(number_of(k), v)) {
                       wrong.fetch_add(1);
                     }
                     previous = current;
                     return true;
                   });
        std::optional<byte_string> next;
        store.scan_rev(bbtree_store::key_range::all(),
                       [&](std::span<const std::uint8_t> k, std::span<const std::uint8_t> v) {
                         const byte_string current(k.begin(), k.end());
                         if ((next && !(current < *next)) || !is_value_of(number_of(k), v)) {
                           wrong.fetch_add(1);
                         }
                         next = current;
                         return true;
                       });
      }
    });
    for (std::size_t w = 0; w < writers; ++w) {
      threads[w].join();
    }
    stop = true;
    for (std::size_t t = writers; t < threads.size(); ++t) {
      threads[t].join();
    }
    CHECK(wrong.load() == 0);
    for (std::size_t n = 0; n < keys; ++n) {
      const std::optional<byte_string> got = store.get(key(n));
      REQUIRE((!got || is_value_of(n, *got)));
    }
  }
  CHECK(tally.out() == 0);
}

TEST_CASE("a range that starts after it ends is refused", "[store]") {
  bbtree_store::bbtree_store<> store;
  const byte_string a{'a'};
  const byte_string b{'b'};
  const auto visit = [](std::span<const std::uint8_t>, std::span<const std::uint8_t>) { return true; };
  CHECK_THROWS_AS(
      store.scan({bbtree_store::key_bound::included(b), bbtree_store::key_bound::included(a)}, visit),
      std::invalid_argument);
  CHECK_THROWS_AS(store.range({bbtree_store::key_bound::excluded(a), bbtree_store::key_bound::excluded(a)}),
                  std::invalid_argument);
  CHECK_NOTHROW(
      store.scan({bbtree_store::key_bound::included(a), bbtree_store::key_bound::excluded(a)}, visit));
}
