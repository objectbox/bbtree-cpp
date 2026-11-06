// Enums whose variants have names on the command line, which reports use
// too (the Rust bench's `Named`), and the error every command reports.
#pragma once

#include <algorithm>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bbtree_bench {

// What a command reports when it fails: the Rust bench's `Err(String)`.
// `bench_program` prints it and exits with a failure.
class bench_error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;

  // Throws `message` unless `condition` holds: the Rust bench's `assert!`,
  // whose panic fails the cell's child process as this does.
  static void unless(bool condition, std::string_view message) {
    if (!condition) {
      throw bench_error(std::string(message));
    }
  }
};

// Each named enum specializes it with `all`, its variants in order, and
// `name(variant)`; enums with more to say about their variants say it there
// too, as the Rust enums' methods do.
template <class E>
struct enum_traits;

template <class E>
class named {
 public:
  static E from_name(std::string_view name) {
    for (const E variant : enum_traits<E>::all) {
      if (enum_traits<E>::name(variant) == name) {
        return variant;
      }
    }
    throw bench_error(std::format("unknown name `{}`; expected one of: {}", name, all_names()));
  }

  // Parses a comma-separated list of names.
  static std::vector<E> from_name_list(std::string_view names) {
    std::vector<E> variants;
    std::size_t start = 0;
    while (true) {
      const std::size_t comma = names.find(',', start);
      variants.push_back(from_name(names.substr(start, comma - start)));
      if (comma == std::string_view::npos) {
        return variants;
      }
      start = comma + 1;
    }
  }

  static std::string all_names() {
    std::string names;
    for (const E variant : enum_traits<E>::all) {
      if (!names.empty()) {
        names += ',';
      }
      names += enum_traits<E>::name(variant);
    }
    return names;
  }

  static std::string_view name(E variant) { return enum_traits<E>::name(variant); }

  // Whether `list` holds `variant`.
  static constexpr bool listed(std::span<const E> list, E variant) noexcept {
    return std::ranges::find(list, variant) != list.end();
  }
};

}  // namespace bbtree_bench
