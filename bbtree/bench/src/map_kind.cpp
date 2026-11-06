#include "map_kind.hpp"

#include "map_adapters.hpp"

namespace bbtree_bench {

// The adapter's list, read at `u64 -> u64`: the lists depend on neither the
// key type nor the value type.
bool enum_traits<map_kind>::supports(map_kind map, operation op) noexcept {
  using key = std::uint64_t;
  using value = std::uint64_t;
  const auto unsupported = [map] {
    switch (map) {
      case map_kind::bbtree:
        return counted_bbtree<key, value>::unsupported();
      case map_kind::unsync:
      case map_kind::unsync_inline:
        return unsync_bbtree<key, value>::unsupported();
      case map_kind::std_btree:
        return std_btree<key, value>::unsupported();
      case map_kind::absl_btree:
        return absl_btree<key, value>::unsupported();
      case map_kind::hashbrown:
        return unordered_dense_hash<key, value>::unsupported();
      case map_kind::std_hash:
        return std_hash<key, value>::unsupported();
    }
    return std::span<const operation>();
  }();
  return !named<operation>::listed(unsupported, op);
}

}  // namespace bbtree_bench
