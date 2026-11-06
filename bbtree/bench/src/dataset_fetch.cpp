#include "dataset_fetch.hpp"

#include <format>
#include <iostream>
#include <system_error>

#include "named.hpp"

namespace bbtree_bench {

void dataset_fetch::fetch_into(const data_directory& directory) {
  install_release(directory);
  for (const data_file file : data_files::all) {
    const data_origin origin = data_files::origin(file);
    if (!origin.from_release) {
      derive_from_upstream(directory, file, origin.tool_arguments);
    }
  }
  directory.verify();
}

void dataset_fetch::install_release(const data_directory& directory) {
  const std::filesystem::path staging = directory.root() / staging_directory;
  std::error_code error;
  if (std::filesystem::exists(staging)) {
    std::filesystem::remove_all(staging, error);
    if (error) {
      throw bench_error(std::format("removing {}: {}", staging.string(), error.message()));
    }
  }
  std::filesystem::create_directories(staging, error);
  if (error) {
    throw bench_error(std::format("creating {}: {}", staging.string(), error.message()));
  }
  std::cerr << std::format("downloading release {} of {} into {}\n", tag, repository, staging.string());
  child_command download("gh");
  download.arg("release").arg("download").arg(std::string(tag)).arg("--repo").arg(std::string(repository));
  download.arg("--dir").arg(staging.string());
  run(download, "log in with `gh auth login`, or set GH_TOKEN, with read access to the repository");
  for (const data_file file : data_files::all) {
    if (data_files::origin(file).from_release) {
      install_data_file(directory, staging, file);
    }
  }
  for (const std::string_view dataset : dataset_directories()) {
    const std::filesystem::path source = staging / std::format("{}.SOURCE.md", dataset);
    const std::filesystem::path destination = directory.root() / dataset / "SOURCE.md";
    create_parent_of(destination);
    std::filesystem::rename(source, destination, error);
    if (error) {
      throw bench_error(
          std::format("moving {} to {}: {}", source.string(), destination.string(), error.message()));
    }
  }
  std::filesystem::remove_all(staging, error);
  if (error) {
    throw bench_error(std::format("removing {}: {}", staging.string(), error.message()));
  }
}

void dataset_fetch::install_data_file(const data_directory& directory, const std::filesystem::path& staging,
                                      data_file file) {
  const std::filesystem::path destination = directory.path_of(file);
  const std::filesystem::path asset = staging / (destination.filename().string() + ".zst");
  create_parent_of(destination);
  std::cerr << "decompressing " << data_files::relative_path(file) << '\n';
  child_command decompress("zstd");
  decompress.arg("--decompress")
      .arg("--quiet")
      .arg("--force")
      .arg(asset.string())
      .arg("-o")
      .arg(destination.string());
  run(decompress, "the release's asset may be damaged");
}

void dataset_fetch::derive_from_upstream(const data_directory& directory, data_file file,
                                         std::span<const std::string_view> tool_arguments) {
  const std::filesystem::path destination = directory.path_of(file);
  create_parent_of(destination);
  std::cerr << "deriving " << data_files::relative_path(file) << " from upstream\n";
  child_command derive("python3");
  derive.arg("-I").arg(std::string(BBTREE_BENCH_SOURCE_DIR) + "/tools/bbtree_datasets.py");
  for (const std::string_view argument : tool_arguments) {
    derive.arg(std::string(argument));
  }
  derive.arg(destination.string());
  run(derive, "the upstream source may be unreachable, or its format may have changed");
}

void dataset_fetch::create_parent_of(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    throw bench_error(std::format("creating {}: {}", path.parent_path().string(), error.message()));
  }
}

std::vector<std::string_view> dataset_fetch::dataset_directories() {
  std::vector<std::string_view> directories;
  for (const data_file file : data_files::all) {
    const std::string_view path = data_files::relative_path(file);
    const std::string_view dataset = path.substr(0, path.find('/'));
    if (directories.empty() || directories.back() != dataset) {
      directories.push_back(dataset);
    }
  }
  return directories;
}

void dataset_fetch::run(const child_command& command, std::string_view hint) {
  child_status status(0);
  try {
    status = command.status();
  } catch (const bench_error& failure) {
    throw bench_error(
        std::format("{}; the dev shell (`nix develop`) of the Rust repository has it", failure.what()));
  }
  if (!status.success()) {
    throw bench_error(std::format("{} exited with {}; {}", command.program(), status.text(), hint));
  }
}

}  // namespace bbtree_bench
