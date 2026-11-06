// `fetch`: installs the datasets in the data directory: the Rust bench's
// `datasets/fetch.rs`, downloading exactly what it downloads. Most data files
// come from the datasets' GitHub release; those that may not be
// redistributed are derived here from their upstream source, by the same
// tool that derived the others.
#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "child_process.hpp"
#include "datasets.hpp"

namespace bbtree_bench {

// The GitHub release that hosts the datasets, and the dataset tool. The
// release's assets are each data file from the release compressed with
// zstd, as `<file name>.zst`; each dataset directory's `SOURCE.md`, as
// `<directory>.SOURCE.md`; and `SHA256SUMS`, which `fetch` doesn't need, as
// it checks the decompressed files against the manifest.
class dataset_fetch {
 public:
  // Installs the release's files and derives the others, then verifies them
  // all as `verify-data` does. Files already there are replaced.
  static void fetch_into(const data_directory& directory);

  // The datasets' directories, which the data files' paths start with. Each
  // has a `SOURCE.md` in the release, those of files derived from upstream
  // too.
  static std::vector<std::string_view> dataset_directories();

 private:
  // `gh` downloads with the user's login, or with `GH_TOKEN` in CI.
  static constexpr std::string_view repository = "XiangpengHao/bbtree";

  // The Rust bench's tag, whose assets this downloads.
  static constexpr std::string_view tag = "bench-data-2026-10-08";

  // Where the assets are downloaded to, inside the data directory, and
  // removed from once installed.
  static constexpr std::string_view staging_directory = ".fetch";

  // Downloads every asset with `gh`, then decompresses the data files with
  // `zstd` and moves the `SOURCE.md` files into the data directory's layout.
  static void install_release(const data_directory& directory);

  // Decompresses `file`'s asset to its place in the data directory.
  static void install_data_file(const data_directory& directory, const std::filesystem::path& staging,
                                data_file file);

  // Derives `file` from upstream with the dataset tool, which downloads the
  // source as it parses it and replaces the file only once it has written
  // all of it.
  static void derive_from_upstream(const data_directory& directory, data_file file,
                                   std::span<const std::string_view> tool_arguments);

  static void create_parent_of(const std::filesystem::path& path);

  // Runs a tool that `fetch` relies on; `hint` says what to try when it
  // fails.
  static void run(const child_command& command, std::string_view hint);
};

}  // namespace bbtree_bench
