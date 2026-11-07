#include "cell_runner.hpp"

#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <variant>

#include "json_objects.hpp"
#include "key_list.hpp"
#include "many_maps.hpp"
#include "map_adapters.hpp"
#include "perf_counters.hpp"
#include "single_map.hpp"
#include "stopwatch.hpp"
#include "text_lines.hpp"

namespace bbtree_bench {

namespace {

// A cell's keys, made or loaded: those of one map, or of many tiny maps.
template <class K>
using cell_keys = std::variant<single_map_keys<K>, many_maps_keys<K>>;

}  // namespace

cell_measurement cell_measurement::parse(std::string_view line) {
  std::map<std::string_view, std::uint64_t> fields;
  for (const std::string_view field : text_lines::split(line, ' ')) {
    if (field.empty()) {
      continue;
    }
    const std::size_t equals = field.find('=');
    const std::optional<std::uint64_t> value =
        equals == std::string_view::npos ? std::nullopt : text_lines::parse_u64(field.substr(equals + 1));
    if (!value) {
      throw bench_error(std::format("malformed measurement `{}`", line));
    }
    fields.emplace(field.substr(0, equals), *value);
  }
  const auto field = [&](std::string_view name) {
    const auto found = fields.find(name);
    if (found == fields.end()) {
      throw bench_error(std::format("no {} in measurement `{}`", name, line));
    }
    return found->second;
  };
  if (fields.contains("operations")) {
    return cell_measurement(timing{field("operations"), field("elapsed_ns"), field("peak_rss_bytes")});
  }
  return cell_measurement(
      memory{field("entries"), field("map_bytes"), field("rss_growth_bytes"), field("peak_rss_bytes")});
}

std::string cell_measurement::to_line() const {
  if (const timing* measured = timed()) {
    return std::format("operations={} elapsed_ns={} peak_rss_bytes={}", measured->operations,
                       measured->elapsed_ns, measured->peak_rss_bytes);
  }
  const memory& measured = of_memory();
  return std::format("entries={} map_bytes={} rss_growth_bytes={} peak_rss_bytes={}", measured.entries,
                     measured.map_bytes, measured.rss_growth_bytes, measured.peak_rss_bytes);
}

double cell_measurement::summary_value() const noexcept {
  if (const timing* measured = timed()) {
    return static_cast<double>(measured->elapsed_ns) / static_cast<double>(measured->operations);
  }
  const memory& measured = of_memory();
  return static_cast<double>(measured.map_bytes) / static_cast<double>(measured.entries);
}

void cell_run::measure_and_report() const {
  const cell& measured = request_.measured();
  const operation op = measured.work().op();
  if (!measured.is_applicable()) {
    throw bench_error(
        std::format("{} lacks {}", named<map_kind>::name(measured.map()), named<operation>::name(op)));
  }
  if (mode_.is_memory() && !enum_traits<operation>::builds_maps(op)) {
    throw bench_error(std::format("memory mode measures builds, not {}", named<operation>::name(op)));
  }
  const cell_measurement measurement = with_key_type();
  std::cout << measurement.to_line() << '\n';
}

cell_measurement cell_run::with_key_type() const {
  const scale run_scale = request_.run_scale();
  // The keys of a generated key kind, in the shape of the cell's size, which
  // is fixed.
  const auto generated = [&]<class K>(key_maker<K> keys) -> cell_keys<K> {
    const std::optional<map_shape> shape = enum_traits<map_size>::fixed_shape(size(), run_scale);
    bench_error::unless(shape.has_value(), "generated keys come in sizes whose shape is fixed");
    if (enum_traits<map_size>::has_many_maps(size())) {
      return many_maps_keys<K>::generated(std::move(keys), *shape);
    }
    return single_map_keys<K>::generated(std::move(keys), shape->entries_per_map(),
                                         enum_traits<scale>::operation_budget(run_scale));
  };
  const auto listed = [&](data_file file) -> cell_keys<std::string> {
    const key_list keys = key_list::load(data_directory::from_environment(), file);
    return keys.single_map_keys_of(size(), run_scale);
  };
  switch (request_.measured().work().key()) {
    case key_kind::u64_random:
      return with_value_type<std::uint64_t>(generated(key_maker<std::uint64_t>::random_u64()));
    case key_kind::json_string:
      return with_value_type<std::string>(generated(key_maker<std::string>::json_keys()));
    case key_kind::crate_string:
      return with_value_type<std::string>(generated(key_maker<std::string>::crate_versions()));
    case key_kind::crate_names:
      return with_value_type<std::string>(listed(data_file::crate_names));
    case key_kind::kernel_paths:
      return with_value_type<std::string>(listed(data_file::kernel_paths));
    case key_kind::wiki_titles:
      return with_value_type<std::string>(listed(data_file::wiki_titles));
    case key_kind::urls:
      return with_value_type<std::string>(listed(data_file::common_crawl_urls));
    case key_kind::addresses:
      return with_value_type<std::string>(listed(data_file::ban_addresses));
    case key_kind::geo_names:
      return with_value_type<std::string>(listed(data_file::geo_names));
    case key_kind::dblp_titles:
      return with_value_type<std::string>(listed(data_file::dblp_titles));
    case key_kind::json_object_keys: {
      json_object_corpus corpus = json_object_corpus::load(data_directory::from_environment());
      const std::size_t map_count =
          enum_traits<scale>::shrink(run_scale, enum_traits<map_size>::real_json_object_maps);
      return with_value_type<std::string>(cell_keys<std::string>(std::move(corpus).repeated_to(map_count)));
    }
  }
  throw bench_error("unknown key kind");
}

template <class K, class Keys>
cell_measurement cell_run::with_value_type(Keys&& keys) const {
  switch (request_.measured().work().value()) {
    case value_kind::u64:
      return with_prepared_cell<K>(std::forward<Keys>(keys), value_maker<std::uint64_t>::counters());
    case value_kind::struct32:
      return with_prepared_cell<K>(std::forward<Keys>(keys), value_maker<struct32>::structs());
  }
  throw bench_error("unknown value kind");
}

// Puts the keys and values in the cell that times them; for
// `bbtree-unsync-compact`, string keys as compact strings first, so that no
// timed operation makes one.
template <class K, class V, class Keys>
cell_measurement cell_run::with_prepared_cell(Keys&& keys, value_maker<V> values) const {
  if constexpr (std::is_same_v<K, std::string>) {
    if (request_.measured().map() == map_kind::unsync_compact) {
      using compact = bbtree::compact_string;
      if (auto* single = std::get_if<single_map_keys<K>>(&keys)) {
        return with_prepared_cell<compact>(
            cell_keys<compact>(single_map_keys<compact>::converted_from(*single)), values);
      }
      return with_prepared_cell<compact>(
          cell_keys<compact>(many_maps_keys<compact>::converted_from(std::get<many_maps_keys<K>>(keys))),
          values);
    }
  }
  if (auto* single = std::get_if<single_map_keys<K>>(&keys)) {
    single_map_cell<K, V> prepared(std::move(*single), values,
                                   enum_traits<scale>::operation_budget(request_.run_scale()));
    return with_map_type<K, V>(prepared);
  }
  many_maps_cell<K, V> prepared(std::move(std::get<many_maps_keys<K>>(keys)), values);
  return with_map_type<K, V>(prepared);
}

template <class K, class V, class Prepared>
cell_measurement cell_run::with_map_type(Prepared& prepared) const {
  if constexpr (std::is_same_v<K, bbtree::compact_string>) {
    bench_error::unless(request_.measured().map() == map_kind::unsync_compact,
                        "compact-string keys are bbtree-unsync-compact's only");
    return measure<unsync_bbtree<K, V>>(prepared);
  } else {
    return with_std_key_map_type<K, V>(prepared);
  }
}

template <class K, class V, class Prepared>
cell_measurement cell_run::with_std_key_map_type(Prepared& prepared) const {
  switch (request_.measured().map()) {
    case map_kind::bbtree:
      return measure<counted_bbtree<K, V>>(prepared);
    case map_kind::unsync:
      return measure<unsync_bbtree<K, V>>(prepared);
    case map_kind::unsync_inline:
      // The integers' inline keys come from their `inline_key`
      // specialization under the default strategy (`map_kind.hpp`).
      if constexpr (std::is_same_v<K, std::uint64_t>) {
        return measure<unsync_bbtree<K, V>>(prepared);
      } else {
        throw bench_error("bbtree-unsync-inline runs on integer keys only");
      }
    case map_kind::unsync_compact:
      throw bench_error("bbtree-unsync-compact runs on string keys only");
    case map_kind::std_btree:
      return measure<std_btree<K, V>>(prepared);
    case map_kind::absl_btree:
      return measure<absl_btree<K, V>>(prepared);
    case map_kind::hashbrown:
      return measure<unordered_dense_hash<K, V>>(prepared);
    case map_kind::std_hash:
      return measure<std_hash<K, V>>(prepared);
  }
  throw bench_error("unknown map kind");
}

template <class M, class Prepared>
cell_measurement cell_run::measure(Prepared& prepared) const {
  const operation op = request_.measured().work().op();
  if (!mode_.is_memory()) {
    std::optional<perf_control> control;
    if (request_.perf_control()) {
      control.emplace(*request_.perf_control());
    }
    stopwatch watch(std::move(control));
    const std::size_t operations = prepared.template time<M>(op, watch);
    return cell_measurement(cell_measurement::timing{
        operations, static_cast<std::uint64_t>(watch.elapsed().count()), resident_set::read().peak_bytes()});
  }
  auto input = prepared.build_input(op);
  const auto entries = static_cast<std::uint64_t>(input.size());
  const std::uint64_t rss_before = resident_set::read().current_bytes();
  std::vector<M> maps = prepared.template build<M>(op, std::move(input));
  const resident_set rss_after = resident_set::read();
  const std::size_t map_bytes = mode_.allocator().bytes_freed_by_dropping(std::move(maps));
  return cell_measurement(cell_measurement::memory{
      entries,
      map_bytes,
      rss_after.current_bytes() > rss_before ? rss_after.current_bytes() - rss_before : 0,
      rss_after.peak_bytes(),
  });
}

}  // namespace bbtree_bench
