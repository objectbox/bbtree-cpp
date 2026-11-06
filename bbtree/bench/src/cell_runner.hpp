// The child side of the suite runner: measures one cell in this process and
// reports it on stdout, as one line of `name=value` fields: the Rust bench's
// `cell_runner.rs`.
//
// The cell's kinds are runtime values; measuring needs them as types. The
// dispatch goes key type, then value type, then map type, and each step's
// `switch` is where a kind maps to its type and its generator or dataset.
// Keys are made or loaded first, before anything is measured.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "command_line.hpp"
#include "memory.hpp"

namespace bbtree_bench {

// What a child measured.
class cell_measurement {
 public:
  struct timing {
    std::uint64_t operations;
    std::uint64_t elapsed_ns;
    std::uint64_t peak_rss_bytes;
  };

  struct memory {
    std::uint64_t entries;
    // Everything the maps hold on the heap, keys' and values' own
    // allocations included, plus the maps' inline headers.
    std::uint64_t map_bytes;
    // How much the resident set grew while the maps were built.
    std::uint64_t rss_growth_bytes;
    std::uint64_t peak_rss_bytes;
  };

  explicit cell_measurement(timing measured) noexcept : measured_(measured) {}
  explicit cell_measurement(memory measured) noexcept : measured_(measured) {}

  static cell_measurement parse(std::string_view line);

  std::string to_line() const;

  // The number summaries report: nanoseconds per operation, or bytes per
  // entry.
  double summary_value() const noexcept;

  const timing* timed() const noexcept { return std::get_if<timing>(&measured_); }
  // The memory measured; throws for a timing.
  const memory& of_memory() const { return std::get<memory>(measured_); }

 private:
  std::variant<timing, memory> measured_;
};

// One cell, measured in this process.
class cell_run {
 public:
  cell_run(cell_request request, run_mode mode) noexcept : request_(std::move(request)), mode_(mode) {}

  void measure_and_report() const;

  // Measures `prepared` on maps of adapter type `M`.
  template <class M, class Prepared>
  cell_measurement measure(Prepared& prepared) const;

 private:
  cell_measurement with_key_type() const;

  template <class K, class Keys>
  cell_measurement with_value_type(Keys&& keys) const;

  template <class K, class V, class Keys>
  cell_measurement with_prepared_cell(Keys&& keys, value_maker<V> values) const;

  template <class K, class V, class Prepared>
  cell_measurement with_map_type(Prepared& prepared) const;

  map_size size() const noexcept { return request_.measured().work().size(); }

  cell_request request_;
  run_mode mode_;
};

}  // namespace bbtree_bench
