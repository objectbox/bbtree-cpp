// The objects of real JSON documents: the Rust bench's
// `datasets/json_objects.rs`. Each becomes one tiny map that holds the
// object's keys, inserted in document order as serde_json does.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "datasets.hpp"
#include "many_maps.hpp"

namespace bbtree_bench {

// The objects of six JSON documents (`data_file::json_object_keys`), with
// each key as an index into their vocabulary
// (`data_file::json_key_vocabulary`).
class json_object_corpus {
 public:
  static json_object_corpus load(const data_directory& directory);

  // Parses the vocabulary, a `key<TAB>count` line per key, and the objects,
  // a line per object with its keys tab-separated (an empty line is an empty
  // object), then checks the counts against the objects.
  static json_object_corpus parse(std::string_view vocabulary_text, std::string_view objects_text);

  // The keys of about `map_count` tiny maps: the corpus, repeated until it
  // has at least that many objects.
  many_maps_keys<std::string> repeated_to(std::size_t map_count) &&;

 private:
  json_object_corpus(std::vector<std::string> vocabulary, picks_per_map objects)
      : vocabulary_(std::move(vocabulary)), objects_(std::move(objects)) {}

  std::vector<std::string> vocabulary_;
  // Each object's keys, in document order.
  picks_per_map objects_;
};

}  // namespace bbtree_bench
