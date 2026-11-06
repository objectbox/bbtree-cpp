// Batches (D53, 12-batch-api.md): many independent keys looked up at once, so
// that one key's misses overlap the others'.
//
// A lookup is a chain of dependent misses, and consecutive lookups don't
// overlap. A batch descends level by level (PORTING.md, P10): every lane
// searches its node at one depth, prefetches the line it reads next, and only
// then does any lane read that line. All leaves lie at one depth, so every
// lane reaches its leaf in the same round, unless it ends early at a
// separator equal to its key or outside what every key shares (D50).
//
// The only software prefetch of the library is here, in `prefetcher`, for the
// line each lane reads next (AGENTS.md, D46's exception). Below an estimated
// L2 of leaf pages, the tree is small enough that one key at a time is as
// fast, and a batch looks its keys up one by one.
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace bbtree::detail {

// The one place that issues software prefetches: the next line a batch's
// lane reads. Never whole keys, unchosen edges, or single-key paths.
struct prefetcher {
  static void line(const void* address) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(address, 0, 3);
#else
    (void)address;
#endif
  }
};

template <class T>
class batch_lookup {
  using K = typename T::key_type;

 public:
  // The most lanes in flight at once; longer batches go in chunks.
  static constexpr std::size_t max_lanes = 64;

  // Whether a tree of `length` entries is large enough for lanes to pay: one
  // L2 of leaf pages at 70% fill (12-batch-api.md, the size cutoff).
  static constexpr bool batches(std::size_t length) noexcept {
    constexpr std::size_t l2_bytes = std::size_t{1} << 20;
    constexpr std::size_t pages_in_l2 = l2_bytes / T::leaf.page.size > 0 ? l2_bytes / T::leaf.page.size : 1;
    constexpr std::size_t entries_per_leaf = T::leaf_capacity * 7 / 10 + 1;
    return length >= pages_in_l2 * entries_per_leaf;
  }

  // Finds each of `keys` in `owner`, writing where each is, or nothing, to
  // the same index of `found`.
  template <class Q>
  static void find_many(const tree<T>& owner, std::size_t length, std::span<const Q* const> keys,
                        std::span<std::optional<found_entry<T>>> found) noexcept {
    if (!owner.has_root() || keys.size() < 2 || owner.root().is_leaf() || !batches(length)) {
      for (std::size_t index = 0; index < keys.size(); ++index) {
        found[index] = owner.find(*keys[index]);
      }
      return;
    }
    for (std::size_t start = 0; start < keys.size(); start += max_lanes) {
      const std::size_t count = keys.size() - start < max_lanes ? keys.size() - start : max_lanes;
      find_chunk(owner, keys.subspan(start, count), found.subspan(start, count));
    }
  }

 private:
  template <class Q>
  static void find_chunk(const tree<T>& owner, std::span<const Q* const> keys,
                         std::span<std::optional<found_entry<T>>> found) noexcept {
    node_ptr<T> nodes[max_lanes];
    std::size_t edges[max_lanes];
    std::size_t live[max_lanes];
    std::size_t live_count = 0;
    const node_ptr<T> root = owner.root();
    for (std::size_t lane = 0; lane < keys.size(); ++lane) {
      found[lane] = std::nullopt;
      const search_key<T, Q> key(*keys[lane]);
      if (owner.shared().place_outside(key)) {
        continue;
      }
      if constexpr (search_key<T, Q>::compares_by_bytes) {
        // The query can miss before the cached root: its window first.
        prefetcher::line(key.bytes().data());
      }
      nodes[lane] = root;
      live[live_count++] = lane;
    }
    // The internal levels: search every lane's node, prefetch the chosen
    // edge's slot; then read every edge, prefetch the child's header.
    while (live_count > 0 && !nodes[live[0]].is_leaf()) {
      std::size_t kept = 0;
      for (std::size_t at = 0; at < live_count; ++at) {
        const std::size_t lane = live[at];
        const internal_node<T> node(nodes[lane]);
        const search_result result = node.search(search_key<T, Q>(*keys[lane]));
        if (result.found) {
          found[lane] = found_entry<T>{node.node(), result.index};
          continue;
        }
        edges[lane] = result.index;
        prefetcher::line(node.edge_slots().slot(result.index));
        live[kept++] = lane;
      }
      live_count = kept;
      for (std::size_t at = 0; at < live_count; ++at) {
        const std::size_t lane = live[at];
        nodes[lane] = internal_node<T>(nodes[lane]).edge(edges[lane]);
        prefetcher::line(nodes[lane].page());
      }
    }
    // The leaves: every header is on its way; the inline keys rule most
    // keys in or out, and a lone candidate's entry is prefetched before the
    // full comparison.
    if constexpr (key_columns<T, true>::has_inline_keys && search_key<T, Q>::compares_inline) {
      for (std::size_t at = 0; at < live_count; ++at) {
        const std::size_t lane = live[at];
        const leaf_node<T> leaf(nodes[lane]);
        const search_key<T, Q> key(*keys[lane]);
        const slot_range candidates =
            leaf.inline_keys().equal_range(key.inline_target(leaf.prefix_len()).inline_key());
        if (candidates.size() == 1) {
          prefetcher::line(leaf.key_address(candidates.start));
        }
      }
    }
    for (std::size_t at = 0; at < live_count; ++at) {
      const std::size_t lane = live[at];
      const leaf_node<T> leaf(nodes[lane]);
      if (const std::optional<std::size_t> index = leaf.find(search_key<T, Q>(*keys[lane]))) {
        found[lane] = found_entry<T>{leaf.node(), *index};
      }
    }
  }
};

}  // namespace bbtree::detail
