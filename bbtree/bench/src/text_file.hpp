// Whole files read as text: the Rust bench's `fs::read_to_string`, for the
// CSVs, perf's counts and `/proc`'s files, whose size is unknown until read.
#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace bbtree_bench {

class text_file {
 public:
  // The file's contents; none if it can't be opened or read.
  static std::optional<std::string> read(const std::filesystem::path& path) {
    const std::unique_ptr<std::FILE, closer> file(std::fopen(path.c_str(), "rb"));
    if (!file) {
      return std::nullopt;
    }
    std::string text;
    std::array<char, 65536> buffer{};
    while (const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file.get())) {
      text.append(buffer.data(), read);
    }
    if (std::ferror(file.get()) != 0) {
      return std::nullopt;
    }
    return text;
  }

 private:
  struct closer {
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
  };
};

}  // namespace bbtree_bench
