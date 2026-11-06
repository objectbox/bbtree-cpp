// The real-world datasets: the manifest that pins each file, the directory
// the files live in, and `verify-data`, which hashes every file against the
// manifest: the Rust bench's `datasets.rs`. `fetch` (`dataset_fetch.hpp`)
// installs them.
//
// Cells read the files through `key_list.hpp` (one key per line) and
// `json_objects.hpp` (the objects of JSON documents). Before reading, a cell
// checks only that the file exists with its pinned size: hashing 400 MB
// would take longer than many cells do.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace bbtree_bench {

// A file of the manifest. Each dataset's directory also holds a
// `SOURCE.md`, which says where the data came from, how it was derived and
// what its keys look like.
enum class data_file {
  // Every crate name on crates.io (database dump of 2026-10-07), sorted.
  crate_names,
  // The regular files of the Linux 6.12 source tree, sorted.
  kernel_paths,
  // English Wikipedia's article titles (namespace 0, dump 20260901), sorted.
  wiki_titles,
  // The keys of every object in six JSON documents from simdjson's examples:
  // one object per line, its keys tab-separated, in document order.
  json_object_keys,
  // The distinct keys of `json_object_keys`, each with its count.
  json_key_vocabulary,
  // The distinct URLs of the pages in random blocks of the index of Common
  // Crawl's crawl CC-MAIN-2023-14, sorted. Derived from the index by `fetch`.
  common_crawl_urls,
  // The distinct addresses of France's Base Adresse Nationale (export of
  // 2026-10-07), as its geocoder labels them, sorted.
  ban_addresses,
  // The distinct names of GeoNames' places (dump of 2026-10-08), sorted.
  geo_names,
  // The distinct titles of dblp's publications (snapshot of 2026-10-03), as
  // plain text, sorted.
  dblp_titles,
};

// What the manifest pins of a data file.
struct pinned_file {
  // Relative to the data directory.
  std::string_view relative_path;
  std::uint64_t bytes;
  std::string_view sha256;
};

// Where `fetch` gets a data file from. Either way, the manifest pins the
// file's exact contents.
struct data_origin {
  // The datasets' GitHub release, or else its upstream source, which never
  // changes, but whose terms don't let the release redistribute the file:
  // `fetch` derives it by running the dataset tool with `tool_arguments`,
  // then the file's path.
  bool from_release;
  std::span<const std::string_view> tool_arguments;
};

class data_files {
 public:
  static constexpr std::array all{
      data_file::crate_names,      data_file::kernel_paths,        data_file::wiki_titles,
      data_file::json_object_keys, data_file::json_key_vocabulary, data_file::common_crawl_urls,
      data_file::ban_addresses,    data_file::geo_names,           data_file::dblp_titles,
  };

  // The manifest, from the datasets' `SOURCE.md` files.
  static pinned_file pinned(data_file file) noexcept;

  static data_origin origin(data_file file) noexcept;

  // The file's path relative to the data directory: its dataset's
  // directory, then its name.
  static std::string_view relative_path(data_file file) noexcept { return pinned(file).relative_path; }
};

// The directory the datasets live in: `$BBTREE_BENCH_DATA`, or else
// `$XDG_CACHE_HOME/bbtree-bench`, or else `~/.cache/bbtree-bench`.
class data_directory {
 public:
  static constexpr std::string_view variable = "BBTREE_BENCH_DATA";

  static data_directory from_environment();

  const std::filesystem::path& root() const noexcept { return root_; }

  std::filesystem::path path_of(data_file file) const { return root_ / data_files::relative_path(file); }

  // Checks that `file` exists with its pinned size, which is all that cells
  // check; `verify-data` checks the contents.
  std::filesystem::path check_present(data_file file) const;

  // Reads `file`, after checking its size.
  std::string read(data_file file) const;

  // `verify-data`: hashes every data file and compares it with the
  // manifest, printing a line per file.
  void verify() const;

 private:
  explicit data_directory(std::filesystem::path root) : root_(std::move(root)) {}

  void verify_file(data_file file) const;

  std::filesystem::path root_;
};

}  // namespace bbtree_bench
