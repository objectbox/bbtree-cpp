// Text as the Rust bench reads it: `str::lines`, `split`, `trim` and
// `parse`, for the datasets, perf's reports, the CSVs and the children's
// measurement lines.
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace bbtree_bench {

class text_lines {
 public:
  // The lines of `text`, as Rust's `str::lines` gives them: split at `\n`,
  // each without a trailing `\r`, and no empty line after a final `\n`.
  static std::vector<std::string_view> of(std::string_view text) {
    std::vector<std::string_view> lines;
    for_each(text, [&](std::string_view line) { lines.push_back(line); });
    return lines;
  }

  // Runs `visit(line)` on each line of `text`, as `of` splits them, without
  // collecting them.
  template <class Visit>
  static void for_each(std::string_view text, Visit&& visit) {
    std::size_t start = 0;
    while (start < text.size()) {
      std::size_t end = text.find('\n', start);
      const std::size_t next = end == std::string_view::npos ? text.size() : end + 1;
      if (end == std::string_view::npos) {
        end = text.size();
      }
      std::string_view line = text.substr(start, end - start);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      visit(line);
      start = next;
    }
  }

  // The fields of `line` between `separator`s, empty ones included.
  static std::vector<std::string_view> split(std::string_view line, char separator) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
      const std::size_t end = line.find(separator, start);
      fields.push_back(
          line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
      if (end == std::string_view::npos) {
        return fields;
      }
      start = end + 1;
    }
  }

  static std::string_view trimmed(std::string_view text) noexcept {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
      return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  }

  static std::optional<double> parse_double(std::string_view text) noexcept {
    double value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || text.empty()) {
      return std::nullopt;
    }
    return value;
  }

  static std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || text.empty()) {
      return std::nullopt;
    }
    return value;
  }
};

}  // namespace bbtree_bench
