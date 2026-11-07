// Key kinds (design/02-field_study.md §6) and their deterministic generators,
// and the key kinds read from real-world datasets (`datasets.hpp`): the Rust
// bench's `keys.rs`. A generator makes the same keys as the Rust one, key
// for key.
#pragma once

#include <bbtree/compact_string.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "datasets.hpp"
#include "map_size.hpp"
#include "named.hpp"
#include "random.hpp"
#include "siphash.hpp"

namespace bbtree_bench {

enum class key_kind {
  // Random `std::uint64_t`s: hashed IDs, cache keys.
  u64_random,
  // Strings as long as JSON object keys (median 7 B, p90 14 B).
  json_string,
  // Strings like crate names with a version (median 16 B).
  crate_string,
  // Every crate name on crates.io (median 12 B, 36% of sorted neighbours
  // share their first 8 bytes).
  crate_names,
  // The file paths of the Linux source tree (median 36 B, 99% of sorted
  // neighbours share their first 8 bytes).
  kernel_paths,
  // English Wikipedia's article titles (median 18 B, 19M of them).
  wiki_titles,
  // The URLs of pages in a Common Crawl crawl, kept as crawled: long keys
  // behind long shared prefixes.
  urls,
  // Every French address, as the Base Adresse Nationale's geocoder labels
  // them (median 39 B, 97% of sorted neighbours share their first 8 bytes,
  // 26M of them).
  addresses,
  // The names of GeoNames' places.
  geo_names,
  // The titles of computer science publications in dblp (median 78 B, 87%
  // of sorted neighbours share their first 8 bytes, 8.4M of them).
  dblp_titles,
  // The keys of the objects of real JSON documents, each object a tiny map
  // (median 2 keys of 8 B).
  json_object_keys,
};

template <>
struct enum_traits<key_kind> {
  static constexpr std::array all{
      key_kind::u64_random,  key_kind::json_string,      key_kind::crate_string,
      key_kind::crate_names, key_kind::kernel_paths,     key_kind::wiki_titles,
      key_kind::urls,        key_kind::addresses,        key_kind::geo_names,
      key_kind::dblp_titles, key_kind::json_object_keys,
  };

  static constexpr std::string_view name(key_kind kind) noexcept {
    switch (kind) {
      case key_kind::u64_random:
        return "u64";
      case key_kind::json_string:
        return "str-json";
      case key_kind::crate_string:
        return "str-16";
      case key_kind::crate_names:
        return "crate-names";
      case key_kind::kernel_paths:
        return "kernel-paths";
      case key_kind::wiki_titles:
        return "wiki-titles";
      case key_kind::urls:
        return "urls";
      case key_kind::addresses:
        return "addresses";
      case key_kind::geo_names:
        return "geonames";
      case key_kind::dblp_titles:
        return "dblp-titles";
      case key_kind::json_object_keys:
        return "json-keys";
    }
    return "";
  }

  // The data files the keys are read from: none for generated keys.
  static std::span<const data_file> data_files(key_kind kind) noexcept;

  static bool is_generated(key_kind kind) noexcept { return data_files(kind).empty(); }

  // Whether the keys are the integers that `bbtree-unsync-inline` runs on.
  static constexpr bool is_inline_keyed_integer(key_kind kind) noexcept {
    return kind == key_kind::u64_random;
  }

  // The sizes the keys come in. Generated keys come in every size whose
  // shape is fixed, and JSON objects in their own shape. The small datasets'
  // keys come in `all` and the fixed sizes they have enough keys for. The
  // large ones' come in 1M and 10M, or `all` instead of 10M when they hold
  // fewer keys than that.
  static std::span<const map_size> sizes(key_kind kind) noexcept;
};

// What the benchmark needs from a key type: the Rust bench's `BenchKey`.
template <class K>
struct key_traits;

template <>
struct key_traits<std::uint64_t> {
  // A cheap read of the key, folded into checksums so that the optimizer
  // can't skip visiting it.
  static std::uint64_t fingerprint(std::uint64_t key) noexcept { return key; }
};

template <>
struct key_traits<std::string> {
  static std::uint64_t fingerprint(const std::string& key) noexcept { return key.size(); }
};

template <class Size, std::size_t ShortCapacity>
struct key_traits<bbtree::basic_compact_string<Size, ShortCapacity>> {
  static std::uint64_t fingerprint(const bbtree::basic_compact_string<Size, ShortCapacity>& key) noexcept {
    return key.size();
  }
};

// How the candidates of one string key kind look: JSON object keys,
// identifier characters with the lengths measured in the field study.
class json_key_shape {
 public:
  std::string candidate(seeded_rng& rng) const {
    const std::size_t length = rng.weighted(length_histogram);
    return rng.ascii_text(length, characters);
  }

 private:
  static constexpr std::string_view characters =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

  // Key lengths in 19k JSON objects from 6 documents: (bytes, keys).
  static constexpr std::array<std::pair<std::size_t, std::uint32_t>, 29> length_histogram{{
      {1, 80},   {2, 1440},  {3, 2628},  {4, 8867},  {5, 6693},  {6, 13055}, {7, 3573},  {8, 12255},
      {9, 2747}, {10, 2597}, {11, 4126}, {12, 2288}, {13, 1785}, {14, 2797}, {15, 1039}, {16, 638},
      {17, 607}, {18, 584},  {19, 449},  {20, 242},  {21, 1254}, {22, 175},  {23, 739},  {24, 207},
      {25, 173}, {26, 173},  {27, 1},    {28, 520},  {34, 173},
  }};
};

// A crate name and a version, `serde 1.0.210`-like (median 16 B, as cargo's
// package IDs).
class crate_version_shape {
 public:
  std::string candidate(seeded_rng& rng) const {
    const std::size_t name_length = rng.in_range(3, 15);
    const std::string name = rng.ascii_text(name_length, name_characters);
    const std::size_t major = rng.below(3);
    const std::size_t minor = rng.below(40);
    const std::size_t patch = rng.below(20);
    return std::format("{} {}.{}.{}", name, major, minor, patch);
  }

 private:
  static constexpr std::string_view name_characters = "abcdefghijklmnopqrstuvwxyz-_";
};

// Strings of one shape, made distinct: a candidate seen before gets random
// identifier characters appended until it is new. Short JSON keys collide
// often (there are only 63^2 two-character ones), the other shapes rarely.
template <class Shape>
class distinct_strings {
 public:
  std::string next_distinct() {
    std::string key = shape_.candidate(rng_);
    // The set holds hashes rather than strings: a hash collision only
    // lengthens a key that was already new, which is harmless. The hash is
    // the one Rust's `DefaultHasher` gives a `str`, so that the same
    // candidates collide as in the Rust bench.
    while (!seen_.insert(sip_hash_13::of_rust_str(key)).second) {
      key += rng_.ascii_text(1, tail_characters);
    }
    return key;
  }

 private:
  static constexpr std::string_view tail_characters =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

  Shape shape_;
  seeded_rng rng_{random_stream::key_content};
  std::unordered_set<std::uint64_t> seen_;
};

// Makes the keys of one kind: distinct indices give distinct keys.
//
// Workloads insert the keys at even indices and use odd ones for keys that
// are never inserted, so that both sets spread over the same key range.
// Makers of random content ignore the index and make a new distinct key at
// every call, which serves the same purpose.
template <class K>
class key_maker {
 public:
  K key(std::uint64_t index) { return make_(index); }

  static key_maker random_u64()
    requires std::is_same_v<K, std::uint64_t>
  {
    return key_maker([](std::uint64_t index) { return seeded_rng::mix(index); });
  }

  static key_maker json_keys()
    requires std::is_same_v<K, std::string>
  {
    return of_distinct_strings(distinct_strings<json_key_shape>());
  }

  static key_maker crate_versions()
    requires std::is_same_v<K, std::string>
  {
    return of_distinct_strings(distinct_strings<crate_version_shape>());
  }

 private:
  explicit key_maker(std::function<K(std::uint64_t)> make) : make_(std::move(make)) {}

  template <class Shape>
  static key_maker of_distinct_strings(distinct_strings<Shape> strings) {
    return key_maker(
        [strings = std::move(strings)](std::uint64_t) mutable { return strings.next_distinct(); });
  }

  std::function<K(std::uint64_t)> make_;
};

}  // namespace bbtree_bench
