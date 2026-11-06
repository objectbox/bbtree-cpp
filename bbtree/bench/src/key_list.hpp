// Datasets of one key per line, and the keys that a one-map cell takes from
// them: the Rust bench's `datasets/key_list.rs`, key for key.
//
// A tenth of the keys are held out: maps never hold them, and lookups that
// miss probe with them. They are chosen by a hash of their bytes
// (SipHash-1-3, as in the Rust bench), so the choice is the same at every
// size and doesn't depend on the file's order.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "datasets.hpp"
#include "map_size.hpp"
#include "single_map.hpp"

namespace bbtree_bench {

class key_list {
 public:
  static key_list load(const data_directory& directory, data_file file) {
    return key_list(file, directory.read(file));
  }

  // A key list of `contents`, as if read from `file`: for the self-check.
  key_list(data_file file, std::string contents) : file_(file), contents_(std::move(contents)) {}

  // The keys of one map of `size`, and the held-out keys that its lookups
  // that miss probe with.
  //
  // - The map holds a seeded sample of the keys that aren't held out, or all
  //   of them at size `all` (a tenth of them under `--quick`). The samples
  //   nest: a smaller map's keys are among a larger one's.
  // - Misses are a seeded sample of the held-out keys, as many as the map has
  //   entries or as the cell's lookups, whichever is fewer.
  single_map_keys<std::string> single_map_keys_of(map_size size, scale run_scale) const;

  // Whether `key` is held out: whether a fixed-key SipHash-1-3 of its bytes
  // is a multiple of `hold_out_one_in`.
  static bool is_held_out(std::string_view key) noexcept;

 private:
  // One key in this many is held out.
  static constexpr std::uint64_t hold_out_one_in = 10;

  // `count` of `keys`: a seeded sample, or all of them, in order. The first
  // steps of the sampling shuffle don't depend on `count`, so a smaller
  // sample is the start of a larger one.
  static std::vector<std::string> sample(std::span<const std::string_view> keys, std::size_t count);

  data_file file_;
  std::string contents_;
};

}  // namespace bbtree_bench
