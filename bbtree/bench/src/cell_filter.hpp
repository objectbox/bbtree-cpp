// Which workloads and maps a run selects: the command line's `--map`,
// `--key`, `--value`, `--size` and `--op` (the Rust bench's `CellFilter`).
#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "workload.hpp"

namespace bbtree_bench {

class argument_cursor;

// An empty list admits everything.
class cell_filter {
 public:
  // An empty list admits every map but the opt-in ones.
  bool admits_map(map_kind map) const noexcept;

  bool admits(const workload& work) const noexcept;

  // Adds the names after a filter flag; throws for a flag that isn't one.
  void parse_flag(std::string_view flag, argument_cursor& cursor);

  // The one cell a filter names, when each of its lists has one entry.
  cell only_cell() const;

 private:
  template <class E>
  static bool list_admits(const std::vector<E>& list, E variant) noexcept {
    return list.empty() || named<E>::listed(list, variant);
  }

  template <class E>
  static E only(const std::vector<E>& list, std::string_view flag);

  std::vector<map_kind> maps_;
  std::vector<key_kind> keys_;
  std::vector<value_kind> values_;
  std::vector<map_size> sizes_;
  std::vector<operation> operations_;
};

}  // namespace bbtree_bench
