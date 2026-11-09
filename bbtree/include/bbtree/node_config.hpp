// Node configs (D59): what a map may tune about its nodes, the sizes of their
// pages, as constants of one type that the map's strategy carries.
//
// A config derives from `default_node_config` and redeclares the constants
// it changes:
//
//   struct large_leaves : bbtree::default_node_config {
//     static constexpr std::size_t leaf_page_size = 4096;
//   };
//   bbtree::unsync::bbtree_map<std::uint64_t, std::uint64_t, std::less<>,
//                              bbtree::configured<bbtree::auto_inline_keys, large_leaves>>
//
// A page size is a power of two. A node is the smallest multiple of its
// kind's page size that holds 12 keys, aligned to a cache line (64 bytes), or
// more if its keys or values need it. Bounds are checked at compile time.
#pragma once

#include <cstddef>

namespace bbtree {

struct default_node_config {
  // Leaves: at least 1024, which bounds how deep a tree grows.
  static constexpr std::size_t leaf_page_size = 1024;
  // Internal nodes of keys with inline keys, which keep only inline keys and
  // edges (D49): at least 64.
  static constexpr std::size_t internal_page_size = 512;
  // Internal nodes of keys without inline keys, which keep their keys: at
  // least 64.
  static constexpr std::size_t plain_internal_page_size = 1024;
  // Heap pages, which hold the bytes a store's or a file map's leaves point
  // to; 0 means the leaves' page size. Maps don't read it.
  static constexpr std::size_t heap_page_size = 0;
};

// Strategies: how a map's keys get inline keys (D18).

// Inline keys for every key that has them (`inline_key.hpp`): std byte
// strings, the 64- and 128-bit integers, and keys with an `inline_key`
// specialization. The default.
struct auto_inline_keys {};

// No inline keys: nodes keep and compare full keys only (D17, D19).
struct plain_keys {};

// Strategy `S`'s inline keys, with nodes built as config `C` says.
template <class S, class C>
struct configured {};

}  // namespace bbtree
