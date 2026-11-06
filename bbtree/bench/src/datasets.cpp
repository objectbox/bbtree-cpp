#include "datasets.hpp"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include "named.hpp"
#include "sha256.hpp"

namespace bbtree_bench {

pinned_file data_files::pinned(data_file file) noexcept {
  switch (file) {
    case data_file::crate_names:
      return {"crate-names/crate-names.txt", 4'754'469,
              "fb27de8ec996495e450f92f77d053b5788d733d09237955b77513b954a2b0bcb"};
    case data_file::kernel_paths:
      return {"linux-paths/linux-6.12-paths.txt", 3'411'142,
              "1250c86563ead7b34d855f96cd753c725a51227e7859ba265f7dbfc63fb55c44"};
    case data_file::wiki_titles:
      return {"wiki-titles/enwiki-20260901-titles.txt", 423'316'037,
              "27997e84a87a3916795548922776dfaa8443cf64a38c6dc7263af2ac933ab63f"};
    case data_file::json_object_keys:
      return {"json-objects/object-keys.tsv", 569'084,
              "aa2ba80a035d168b65fe8a9ddf7effdb667604ed62c2b9f77ddb9fac31f86c1e"};
    case data_file::json_key_vocabulary:
      return {"json-objects/key-vocabulary.tsv", 17'999,
              "fbd52a705ce03bf94012d3cf932f3065245354afc620217b3ebad907194b546c"};
    // TODO(phase 2, as in the Rust bench): pin the derived files.
    case data_file::common_crawl_urls:
      return {"cc-urls/cc-main-2023-14-urls.txt", 0,
              "0000000000000000000000000000000000000000000000000000000000000000"};
    case data_file::ban_addresses:
      return {"ban-addresses/ban-2026-10-07-addresses.txt", 1'064'160'095,
              "18072059a36e95e2c10f5871da1d1494c606b487d27497cfabaa8704e7e361de"};
    case data_file::geo_names:
      return {"geonames/geonames-2026-10-08-names.txt", 0,
              "0000000000000000000000000000000000000000000000000000000000000000"};
    case data_file::dblp_titles:
      return {"dblp-titles/dblp-2026-10-03-titles.txt", 684'074'119,
              "b9c22b15b3075aec603b1310f9ab7e9a32a22e094929a2f857e699184b5ce11c"};
  }
  return {};
}

data_origin data_files::origin(data_file file) noexcept {
  static constexpr std::array<std::string_view, 5> common_crawl_tool_arguments{
      "cc-urls",
      "https://data.commoncrawl.org/cc-index/collections/CC-MAIN-2023-14/indexes/cluster.idx",
      "https://data.commoncrawl.org/cc-index/collections/CC-MAIN-2023-14/indexes",
      "4500",
      "42",
  };
  if (file == data_file::common_crawl_urls) {
    return {false, common_crawl_tool_arguments};
  }
  return {true, {}};
}

data_directory data_directory::from_environment() {
  const auto environment_path = [](const char* name) -> std::optional<std::filesystem::path> {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
      return std::nullopt;
    }
    return std::filesystem::path(value);
  };
  const std::string own_variable(variable);
  if (const auto root = environment_path(own_variable.c_str())) {
    return data_directory(*root);
  }
  if (const auto cache = environment_path("XDG_CACHE_HOME")) {
    return data_directory(*cache / "bbtree-bench");
  }
  if (const auto home = environment_path("HOME")) {
    return data_directory(*home / ".cache/bbtree-bench");
  }
  throw bench_error(std::format("neither {}, XDG_CACHE_HOME nor HOME is set, so there is no data directory",
                                data_directory::variable));
}

std::filesystem::path data_directory::check_present(data_file file) const {
  const std::filesystem::path path = path_of(file);
  std::error_code error;
  const std::uintmax_t bytes = std::filesystem::file_size(path, error);
  if (error) {
    throw bench_error(std::format("{}: {}", path.string(), error.message()));
  }
  const std::uint64_t pinned_bytes = data_files::pinned(file).bytes;
  if (bytes != pinned_bytes) {
    throw bench_error(
        std::format("{}: {} bytes, but the manifest pins {}", path.string(), bytes, pinned_bytes));
  }
  return path;
}

std::string data_directory::read(data_file file) const {
  const std::filesystem::path path = check_present(file);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw bench_error(std::format("reading {}: cannot open", path.string()));
  }
  std::string contents(static_cast<std::size_t>(data_files::pinned(file).bytes), '\0');
  stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (static_cast<std::size_t>(stream.gcount()) != contents.size()) {
    throw bench_error(std::format("reading {}: short read", path.string()));
  }
  return contents;
}

void data_directory::verify() const {
  std::cout << "data directory: " << root_.string() << '\n';
  std::size_t failures = 0;
  for (const data_file file : data_files::all) {
    try {
      verify_file(file);
      std::cout << "ok      " << data_files::relative_path(file) << '\n';
    } catch (const bench_error& problem) {
      ++failures;
      std::cout << "FAILED  " << problem.what() << '\n';
    }
  }
  if (failures > 0) {
    throw bench_error(
        std::format("{} of {} data files failed verification; `bbtree-bench fetch` downloads them", failures,
                    data_files::all.size()));
  }
}

void data_directory::verify_file(data_file file) const {
  const std::filesystem::path path = check_present(file);
  const std::string digest = sha256::hex_digest_of_file(path);
  const std::string_view pinned_digest = data_files::pinned(file).sha256;
  if (digest != pinned_digest) {
    throw bench_error(
        std::format("{}: SHA-256 {}, but the manifest pins {}", path.string(), digest, pinned_digest));
  }
}

}  // namespace bbtree_bench
