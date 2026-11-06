#include "key_list.hpp"

#include <algorithm>
#include <format>
#include <numeric>

#include "parallel_setup.hpp"
#include "random.hpp"
#include "siphash.hpp"
#include "text_lines.hpp"

namespace bbtree_bench {

bool key_list::is_held_out(std::string_view key) noexcept {
  return sip_hash_13::of_bytes(key) % hold_out_one_in == 0;
}

single_map_keys<std::string> key_list::single_map_keys_of(map_size size, scale run_scale) const {
  std::vector<std::string_view> in_set;
  std::vector<std::string_view> held_out;
  text_lines::for_each(contents_,
                       [&](std::string_view key) { (is_held_out(key) ? held_out : in_set).push_back(key); });
  // Sorted first, so that the samples don't depend on the file's order.
  parallel_setup::sort(in_set, std::less<>());
  parallel_setup::sort(held_out, std::less<>());
  std::size_t entries = 0;
  if (size == map_size::all) {
    entries = enum_traits<scale>::shrink(run_scale, in_set.size());
  } else {
    const std::optional<map_shape> shape = enum_traits<map_size>::fixed_shape(size, run_scale);
    if (!shape || shape->map_count() != 1) {
      throw bench_error(std::format("key lists make one map, not {}", named<map_size>::name(size)));
    }
    entries = shape->entries_per_map();
  }
  if (entries > in_set.size()) {
    throw bench_error(std::format("{} has {} keys that aren't held out, fewer than a {} map holds",
                                  data_files::relative_path(file_), in_set.size(),
                                  named<map_size>::name(size)));
  }
  const std::size_t miss_count =
      std::min({entries, enum_traits<scale>::operation_budget(run_scale), held_out.size()});
  return single_map_keys<std::string>(sample(in_set, entries), sample(held_out, miss_count));
}

std::vector<std::string> key_list::sample(std::span<const std::string_view> keys, std::size_t count) {
  std::vector<std::size_t> chosen;
  if (count < keys.size()) {
    chosen = seeded_rng(random_stream::dataset_sample).choose_indices(keys.size(), count);
  } else {
    chosen.resize(count);
    std::iota(chosen.begin(), chosen.end(), std::size_t{0});
  }
  return parallel_setup::map(std::span<const std::size_t>(chosen),
                             [keys](std::size_t, std::size_t index) { return std::string(keys[index]); });
}

}  // namespace bbtree_bench
