#include "json_objects.hpp"

#include <cstdint>
#include <format>
#include <unordered_map>

#include "text_lines.hpp"

namespace bbtree_bench {

json_object_corpus json_object_corpus::load(const data_directory& directory) {
  const std::string vocabulary = directory.read(data_file::json_key_vocabulary);
  const std::string objects = directory.read(data_file::json_object_keys);
  return parse(vocabulary, objects);
}

json_object_corpus json_object_corpus::parse(std::string_view vocabulary_text,
                                             std::string_view objects_text) {
  // Both files escape `\`, tab, LF and CR with a backslash; the pinned files
  // have no such key, so there is nothing to unescape.
  if (vocabulary_text.find('\\') != std::string_view::npos ||
      objects_text.find('\\') != std::string_view::npos) {
    throw bench_error("JSON keys with escapes aren't supported");
  }
  std::vector<std::string> vocabulary;
  std::vector<std::size_t> counts;
  for (const std::string_view line : text_lines::of(vocabulary_text)) {
    const std::size_t tab = line.find('\t');
    const std::optional<std::uint64_t> count =
        tab == std::string_view::npos ? std::nullopt : text_lines::parse_u64(line.substr(tab + 1));
    if (!count) {
      throw bench_error(std::format("vocabulary line `{}` has no count", line));
    }
    vocabulary.emplace_back(line.substr(0, tab));
    counts.push_back(static_cast<std::size_t>(*count));
  }
  std::unordered_map<std::string_view, std::uint16_t> picks_by_key;
  for (std::size_t index = 0; index < vocabulary.size(); ++index) {
    if (index > UINT16_MAX) {
      throw bench_error("the vocabulary has over 65536 keys");
    }
    if (!picks_by_key.emplace(vocabulary[index], static_cast<std::uint16_t>(index)).second) {
      throw bench_error(std::format("the vocabulary lists `{}` twice", vocabulary[index]));
    }
  }

  picks_per_map objects;
  std::vector<std::uint16_t> object_picks;
  for (const std::string_view line : text_lines::of(objects_text)) {
    object_picks.clear();
    if (!line.empty()) {
      for (const std::string_view key : text_lines::split(line, '\t')) {
        const auto found = picks_by_key.find(key);
        if (found == picks_by_key.end()) {
          throw bench_error(std::format("object key `{}` isn't in the vocabulary", key));
        }
        object_picks.push_back(found->second);
      }
    }
    objects.push_map(object_picks);
  }
  if (objects.pick_counts(vocabulary.size()) != counts) {
    throw bench_error("the vocabulary's counts don't match the objects");
  }
  return json_object_corpus(std::move(vocabulary), std::move(objects));
}

many_maps_keys<std::string> json_object_corpus::repeated_to(std::size_t map_count) && {
  const std::size_t copies = (map_count + objects_.map_count() - 1) / objects_.map_count();
  picks_per_map maps =
      picks_per_map::with_capacity(copies * objects_.map_count(), copies * objects_.pick_count());
  for (std::size_t copy = 0; copy < copies; ++copy) {
    for (std::size_t object = 0; object < objects_.map_count(); ++object) {
      maps.push_map(objects_.map_picks(object));
    }
  }
  return many_maps_keys<std::string>(std::move(vocabulary_), std::move(maps));
}

}  // namespace bbtree_bench
