#include "self_check.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "key_list.hpp"
#include "keys.hpp"
#include "many_maps.hpp"
#include "random.hpp"
#include "sha256.hpp"
#include "single_map.hpp"
#include "siphash.hpp"
#include "values.hpp"

// The pinned digests come from the Rust bench at the commit PLAN.md names,
// with a module added to its own sources (so that it reaches their private
// items) that digests the same sequences of the same cells the same way:
// FNV-1a over each sequence, an integer as its 8 little-endian bytes, a
// string as its length encoded so, then its bytes (bbtree/bench/README.md,
// "Checking the keys against the Rust bench"). If the Rust bench's
// generators change, its digests change, and this check says so.

namespace bbtree_bench {

namespace {

// FNV-1a, 64 bits, over a sequence's canonical bytes.
class sequence_digest {
 public:
  void add(std::uint64_t number) noexcept {
    for (int byte = 0; byte < 8; ++byte) {
      add_byte(static_cast<std::uint8_t>(number >> (8 * byte)));
    }
  }

  void add(const std::string& text) noexcept {
    add(static_cast<std::uint64_t>(text.size()));
    for (const char character : text) {
      add_byte(static_cast<std::uint8_t>(character));
    }
  }

  void add(const struct32& value) noexcept {
    for (const std::uint64_t field : value) {
      add(field);
    }
  }

  std::uint64_t value() const noexcept { return state_; }

 private:
  void add_byte(std::uint8_t byte) noexcept {
    state_ ^= byte;
    state_ *= 0x100'0000'01b3;
  }

  std::uint64_t state_ = 0xcbf2'9ce4'8422'2325;
};

// A sequence's digest as the check prints and pins it.
struct pinned_sequence {
  std::string_view label;
  std::uint64_t count;
  std::uint64_t digest;
};

// What the Rust bench gives (see the top of this file).
constexpr std::array<pinned_sequence, 50> rust_sequences{{
    {"u64 1k insertion order", 1000, 0xb5e69b80e40840f0},
    {"u64 1k absent", 1000, 0x9c368ec49c9696d5},
    {"u64 1k hit probes", 1000000, 0xe96e71d8f6339989},
    {"u64 1k miss probes", 1000000, 0x017adf2fd1243944},
    {"u64 1k range probes", 1000000, 0x9da9abafe9f2dba7},
    {"u64 1k removal order", 1000, 0x90bdf28d1f8dd35c},
    {"u64 1k upsert order", 2000, 0xfa3d21129ad8ecc9},
    {"str-json 1k insertion order", 1000, 0xcade8e3c3a7e0727},
    {"str-json 1k absent", 1000, 0x7d9ad65e9509351e},
    {"str-json 1k hit probes", 1000000, 0x2654e16c0ab50f00},
    {"str-json 1k miss probes", 1000000, 0xdae13be5bc683b30},
    {"str-json 1k range probes", 1000000, 0xf80e7c9e141466ad},
    {"str-json 1k removal order", 1000, 0x7ddaadfeb5454199},
    {"str-json 1k upsert order", 2000, 0xf02497a68b768ef1},
    {"str-16 1k insertion order", 1000, 0x957ce14c2e8f274c},
    {"str-16 1k absent", 1000, 0xed0afd934a7fec52},
    {"str-16 1k hit probes", 1000000, 0x2ad0dd34fe83376d},
    {"str-16 1k miss probes", 1000000, 0x3c4c9872ffefdb32},
    {"str-16 1k range probes", 1000000, 0xaf35f336ca749513},
    {"str-16 1k removal order", 1000, 0x9745f4cc652fa90a},
    {"str-16 1k upsert order", 2000, 0xeeba4e5d12f12609},
    {"u64 1M quick insertion order", 100000, 0x494c094451408690},
    {"u64 1M quick absent", 100000, 0x4aaaa9b1da327f0a},
    {"u64 1M quick hit probes", 100000, 0x26a5b76774483f47},
    {"u64 1M quick miss probes", 100000, 0xf4de13ca276270d7},
    {"u64 1M quick range probes", 100000, 0xb4cbf3dd68607e30},
    {"u64 1M quick removal order", 100000, 0x64c9f393156003f8},
    {"u64 1M quick upsert order", 200000, 0x4656a0104dc5c7d5},
    {"str-json 1M quick insertion order", 100000, 0x6eeaf39b4be85756},
    {"str-json 1M quick absent", 100000, 0xb8c0d8a2c73b3cf6},
    {"str-json 1M quick hit probes", 100000, 0x71dea65dff6098f5},
    {"str-json 1M quick miss probes", 100000, 0x028cc6c2a4c0d8be},
    {"str-json 1M quick range probes", 100000, 0xc13caf98afe6032b},
    {"str-json 1M quick removal order", 100000, 0xd80765666688016a},
    {"str-json 1M quick upsert order", 200000, 0x8df71c065e0f22d5},
    {"str-json 1Mx4 quick vocabulary", 256, 0x4d3dae1c594e6619},
    {"str-json 1Mx4 quick picks", 400000, 0x8862e2b921508f67},
    {"str-json 1Mx4 quick map sizes", 100000, 0x2aae72d313d79f25},
    {"str-json 1Mx4 quick absent picks", 400000, 0xa9a5016947a1b8d3},
    {"str-json 1Mx4 quick map order", 100000, 0x9b5b01d4ac62328d},
    {"u64 100kx32 quick vocabulary", 256, 0x6bd58b054ba240e1},
    {"u64 100kx32 quick picks", 320000, 0x5db066e306b27f66},
    {"u64 100kx32 quick map sizes", 10000, 0xa481a02654764b25},
    {"u64 100kx32 quick absent picks", 320000, 0x74e0a1b971616c60},
    {"u64 100kx32 quick map order", 10000, 0xa7b216e9ba16c585},
    {"key-list 1k present", 1000, 0xbc0a9667e358113e},
    {"key-list 1k absent", 1000, 0xfb16e2b9ad4333b0},
    {"key-list all present", 17958, 0xcd2d158e4e26b20b},
    {"key-list all absent", 2042, 0x7c108c640ca6b0ef},
    {"struct32 values", 1000, 0x6906158b32478fc4},
}};

// The checks, run and compared one at a time.
class sequence_checks {
 public:
  template <class T>
  void sequence(const std::string& label, const std::vector<T>& items) {
    sequence_digest digest;
    for (const T& item : items) {
      digest.add(item);
    }
    report(label, items.size(), digest.value());
  }

  void equal(std::string_view label, std::uint64_t actual, std::uint64_t expected) {
    const bool same = actual == expected;
    std::cout << std::format("{}  {} = {:#018x}\n", same ? "ok    " : "FAILED", label, actual);
    failures_ += same ? 0 : 1;
  }

  void equal(std::string_view label, std::string_view actual, std::string_view expected) {
    const bool same = actual == expected;
    std::cout << std::format("{}  {} = {}\n", same ? "ok    " : "FAILED", label, actual);
    failures_ += same ? 0 : 1;
  }

  std::size_t failures() const noexcept { return failures_; }

  // The orders of a one-map cell of generated keys.
  template <class K>
  void single_map(std::string_view kind, key_maker<K> keys, map_size size, scale run_scale) {
    const map_shape shape = *enum_traits<map_size>::fixed_shape(size, run_scale);
    const std::size_t budget = enum_traits<scale>::operation_budget(run_scale);
    const single_map_cell<K, std::uint64_t> cell(
        single_map_keys<K>::generated(std::move(keys), shape.entries_per_map(), budget),
        value_maker<std::uint64_t>::counters(), budget);
    const std::string prefix = cell_prefix(kind, size, run_scale);
    cell.for_each_order(
        [&](std::string_view order, const auto& items) { sequence(prefix + std::string(order), items); });
  }

  // The orders of a cell of many maps of generated keys.
  template <class K>
  void many_maps(std::string_view kind, key_maker<K> keys, map_size size, scale run_scale) {
    const map_shape shape = *enum_traits<map_size>::fixed_shape(size, run_scale);
    const many_maps_cell<K, struct32> cell(many_maps_keys<K>::generated(std::move(keys), shape),
                                           value_maker<struct32>::structs());
    const std::string prefix = cell_prefix(kind, size, run_scale);
    cell.for_each_order(
        [&](std::string_view order, const auto& items) { sequence(prefix + std::string(order), items); });
  }

 private:
  static std::string cell_prefix(std::string_view kind, map_size size, scale run_scale) {
    return std::format("{} {}{} ", kind, named<map_size>::name(size),
                       run_scale == scale::quick ? " quick" : "");
  }

  void report(const std::string& label, std::size_t count, std::uint64_t digest) {
    const pinned_sequence* pinned = nullptr;
    for (const pinned_sequence& candidate : rust_sequences) {
      if (candidate.label == label) {
        pinned = &candidate;
      }
    }
    const bool same = pinned != nullptr && pinned->count == count && pinned->digest == digest;
    std::cout << std::format("{}  {} {} {:016x}\n", same ? "ok    " : "FAILED", label, count, digest);
    failures_ += same ? 0 : 1;
  }

  std::size_t failures_ = 0;
};

}  // namespace

void self_check::run() {
  sequence_checks checks;

  // The hashes the Rust bench pins (`key_list.rs`'s tests), and SHA-256's
  // test vector.
  checks.equal("siphash13(\"\")", sip_hash_13::of_bytes(""), 0xd1fb'a762'150c'532c);
  checks.equal("siphash13(\"serde\")", sip_hash_13::of_bytes("serde"), 0xe88e'ff3c'6215'a6f9);
  checks.equal("sha256(\"abc\")", sha256::hex_digest(std::array<std::uint8_t, 3>{'a', 'b', 'c'}),
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // One-map cells: small and quick ones, the latter of more keys than the
  // Rust bench shuffles on all cores.
  checks.single_map("u64", key_maker<std::uint64_t>::random_u64(), map_size::one_thousand, scale::full);
  checks.single_map("str-json", key_maker<std::string>::json_keys(), map_size::one_thousand, scale::full);
  checks.single_map("str-16", key_maker<std::string>::crate_versions(), map_size::one_thousand, scale::full);
  checks.single_map("u64", key_maker<std::uint64_t>::random_u64(), map_size::one_million, scale::quick);
  checks.single_map("str-json", key_maker<std::string>::json_keys(), map_size::one_million, scale::quick);

  // Many-map cells.
  checks.many_maps("str-json", key_maker<std::string>::json_keys(), map_size::four_entry_maps, scale::quick);
  checks.many_maps("u64", key_maker<std::uint64_t>::random_u64(), map_size::thirty_two_entry_maps,
                   scale::quick);

  // A key list: crate-like names, as the Rust bench's tests make them.
  std::string names;
  for (int index = 0; index < 20'000; ++index) {
    names += std::format("crate-{:06}\n", index);
  }
  const key_list crate_like(data_file::crate_names, names);
  for (const map_size size : {map_size::one_thousand, map_size::all}) {
    const single_map_keys<std::string> keys = crate_like.single_map_keys_of(size, scale::full);
    const std::string prefix = std::format("key-list {}", named<map_size>::name(size));
    checks.sequence(prefix + " present", keys.present());
    checks.sequence(prefix + " absent", keys.absent());
  }

  // Values.
  const value_maker<struct32> structs = value_maker<struct32>::structs();
  std::vector<struct32> values;
  for (std::uint64_t index = 0; index < 1000; ++index) {
    values.push_back(structs.value(index));
  }
  checks.sequence("struct32 values", values);

  if (checks.failures() > 0) {
    throw bench_error(std::format("{} checks differ from the Rust bench", checks.failures()));
  }
  std::cout << "every sequence is the Rust bench's\n";
}

}  // namespace bbtree_bench
