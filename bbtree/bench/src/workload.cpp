#include "workload.hpp"

#include <format>

namespace bbtree_bench {

workload::workload(key_kind key, value_kind value, map_size size, operation op)
    : key_(key), value_(value), size_(size), op_(op) {
  bench_error::unless(
      named<map_size>::listed(enum_traits<key_kind>::sizes(key), size),
      std::format("{} keys don't come in size {}", named<key_kind>::name(key), named<map_size>::name(size)));
  bench_error::unless(
      enum_traits<operation>::applies_to(op, size),
      std::format("{} does not apply to size {}", named<operation>::name(op), named<map_size>::name(size)));
}

std::vector<workload> workload::product(std::span<const key_kind> keys, std::span<const value_kind> values,
                                        std::span<const map_size> sizes,
                                        std::span<const operation> operations) {
  std::vector<workload> workloads;
  for (const key_kind key : keys) {
    for (const value_kind value : values) {
      for (const map_size size : sizes) {
        for (const operation op : operations) {
          workloads.emplace_back(key, value, size, op);
        }
      }
    }
  }
  return workloads;
}

std::string workload::name() const {
  return std::format("{}_{}_{}_{}", named<key_kind>::name(key_), named<value_kind>::name(value_),
                     named<operation>::name(op_), named<map_size>::name(size_));
}

std::string cell::label() const {
  return std::format("{} {}", named<map_kind>::name(map_), workload_.name());
}

bool cell::is_applicable() const noexcept {
  return enum_traits<map_kind>::supports(map_, workload_.op()) &&
         enum_traits<map_kind>::supports_key(map_, workload_.key());
}

}  // namespace bbtree_bench
