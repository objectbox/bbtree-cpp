// What one measurement runs: a workload (key kind x value kind x size x
// operation) on one map, at full or quick scale: the Rust bench's
// `workload.rs`.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "keys.hpp"
#include "map_kind.hpp"
#include "map_size.hpp"
#include "operation.hpp"
#include "values.hpp"

namespace bbtree_bench {

// One row of the report: what runs on every map.
class workload {
 public:
  // Throws if the keys don't come in this size, or the operation doesn't
  // apply to maps of this size.
  workload(key_kind key, value_kind value, map_size size, operation op);

  // Every combination of the given kinds, sizes and operations, nested in
  // that order.
  static std::vector<workload> product(std::span<const key_kind> keys, std::span<const value_kind> values,
                                       std::span<const map_size> sizes,
                                       std::span<const operation> operations);

  key_kind key() const noexcept { return key_; }
  value_kind value() const noexcept { return value_; }
  map_size size() const noexcept { return size_; }
  operation op() const noexcept { return op_; }

  // The benchmark's name in reports: key, value, operation and size, as in
  // `u64_u64_random_insert_10M`.
  std::string name() const;

  friend bool operator==(const workload&, const workload&) = default;

 private:
  key_kind key_;
  value_kind value_;
  map_size size_;
  operation op_;
};

// One workload on one map: the unit the suite runner starts a child process
// for, once per repetition.
class cell {
 public:
  cell(map_kind map, workload work) noexcept : map_(map), workload_(work) {}

  map_kind map() const noexcept { return map_; }
  const workload& work() const noexcept { return workload_; }

  // The map and the benchmark, as progress lines print them.
  std::string label() const;

  // Whether the map has the API the operation needs, and runs on the keys.
  // Cells that aren't applicable are reported as `n/a`, not run.
  bool is_applicable() const noexcept;

 private:
  map_kind map_;
  workload workload_;
};

}  // namespace bbtree_bench
