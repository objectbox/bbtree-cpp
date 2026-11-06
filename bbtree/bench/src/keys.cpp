#include "keys.hpp"

namespace bbtree_bench {

std::span<const data_file> enum_traits<key_kind>::data_files(key_kind kind) noexcept {
  static constexpr std::array crate_names{data_file::crate_names};
  static constexpr std::array kernel_paths{data_file::kernel_paths};
  static constexpr std::array wiki_titles{data_file::wiki_titles};
  static constexpr std::array urls{data_file::common_crawl_urls};
  static constexpr std::array addresses{data_file::ban_addresses};
  static constexpr std::array geo_names{data_file::geo_names};
  static constexpr std::array dblp_titles{data_file::dblp_titles};
  static constexpr std::array json_objects{data_file::json_object_keys, data_file::json_key_vocabulary};
  switch (kind) {
    case key_kind::u64_random:
    case key_kind::json_string:
    case key_kind::crate_string:
      return {};
    case key_kind::crate_names:
      return crate_names;
    case key_kind::kernel_paths:
      return kernel_paths;
    case key_kind::wiki_titles:
      return wiki_titles;
    case key_kind::urls:
      return urls;
    case key_kind::addresses:
      return addresses;
    case key_kind::geo_names:
      return geo_names;
    case key_kind::dblp_titles:
      return dblp_titles;
    case key_kind::json_object_keys:
      return json_objects;
  }
  return {};
}

std::span<const map_size> enum_traits<key_kind>::sizes(key_kind kind) noexcept {
  static constexpr std::array small_datasets{map_size::one_thousand, map_size::all};
  static constexpr std::array large_datasets{map_size::ten_thousand, map_size::one_million,
                                             map_size::ten_million};
  static constexpr std::array datasets_under_ten_million{map_size::one_million, map_size::all};
  static constexpr std::array json_objects{map_size::real_json_objects};
  switch (kind) {
    case key_kind::crate_names:
    case key_kind::kernel_paths:
      return small_datasets;
    case key_kind::wiki_titles:
    case key_kind::urls:
    case key_kind::addresses:
      return large_datasets;
    // TODO(phase 2, as in the Rust bench): GeoNames' count decides whether
    // 10M or `all`.
    case key_kind::geo_names:
    case key_kind::dblp_titles:
      return datasets_under_ten_million;
    case key_kind::json_object_keys:
      return json_objects;
    case key_kind::u64_random:
    case key_kind::json_string:
    case key_kind::crate_string:
      return enum_traits<map_size>::fixed_shapes;
  }
  return {};
}

}  // namespace bbtree_bench
