// Reads of a latched tree's nodes while other threads write them: internal
// nodes always, by optimistic lock coupling, and leaves for lookups (D57;
// 09-concurrency.md). A reader takes no latch: it reads the node's version,
// reads the node in place, and checks the version after; a writer holding the
// node changes it in place with the ordinary node code. So a racing read here
// - reads the node's length and prefix once each, volatile, and clamps the
//   length to the capacity, so that any bytes give an in-bounds search (D14);
// - acts on nothing it read before the node's version vouches for it, and
//   follows no edge before that;
// - runs no user code on what it read: where inline keys tie, it compares the
//   separator's key, a leaf head (D49), with that leaf latched shared; keys
//   ordered by their own bytes compare as an unlatched copy instead, once the
//   version vouches for it;
// - is a formal data race, as in the Rust crate and the literature (Leis et
//   al.); ThreadSanitizer is told to ignore it (PORTING.md, P11).
#pragma once

#include <bbtree/detail/latched/latch.hpp>
#include <bbtree/detail/node/internal.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstring>
#include <optional>
#include <type_traits>

namespace bbtree::detail {

// Where routing through an internal node went: its edge, whether it matched a
// separator there, and the node's length as read.
struct routed {
  std::size_t edge;
  bool at_separator;
  std::size_t len;
};

// What an unlatched look at a leaf's inline keys told: the key is absent, or
// has one candidate, or the leaf must be searched latched.
struct leaf_candidate {
  enum class kind : std::uint8_t { absent, one, unknown };
  kind found;
  std::size_t index;
  std::size_t prefix_len;
};

template <class T>
class racing_node_reads {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

 public:
  // Keys ordered by their own bytes alone, every bit pattern of them a key:
  // the std integers and byte arrays. A torn copy of one compares without
  // reading anything else.
  static constexpr bool orders_by_own_bytes =
      T::inliner::kind == inliner_kind::array_bytes ||
      (T::inliner::kind == inliner_kind::trait &&
       (std::is_integral_v<K> || std::is_same_v<K, int128> || std::is_same_v<K, uint128>));

  // The node's length, clamped to `capacity`, and prefix length.
  static std::pair<std::size_t, std::size_t> header(node_ptr<T> node, std::size_t capacity) noexcept {
    const racing_reads reads;
    const auto* header = reinterpret_cast<const volatile inline_key_node_header*>(node.page());
    const std::size_t len = header->header.len;
    const std::size_t prefix_len = header->prefix_len;
    return {len < capacity ? len : capacity, prefix_len};
  }

  // The child at `edge`, read without a latch; null for an empty slot.
  static node_ptr<T> child(internal_node<T> node, std::size_t edge) noexcept {
    const racing_reads reads;
    const auto* slot = reinterpret_cast<const volatile std::uintptr_t*>(node.edge_slots().slot(edge));
    return node_ptr<T>::from_tagged(*slot);
  }

  // Routes `key` through `node`, read at `version`; none if a check failed.
  // Inlined, as `candidate` is: the descent's and the lookups' loops are
  // these two (`latched_tree::descend`).
  template <class Q>
  [[gnu::always_inline]] static std::optional<routed> route(internal_node<T> node,
                                                            const search_key<T, Q>& key,
                                                            latch_version version) noexcept {
    std::size_t low = 0;
    std::size_t high = 0;
    std::size_t len = 0;
    window target{};
    {
      const racing_reads reads;
      const auto [read_len, prefix_len] = header(node.node(), internal_node<T>::capacity);
      len = read_len;
      if constexpr (search_key<T, Q>::compares_inline) {
        target = key.inline_target(prefix_len);
        const slot_range candidates =
            key_columns<T, false>(node.node().page()).inline_keys(len).equal_range(target.inline_key());
        if (candidates.empty()) {
          return routed{candidates.start, false, len};
        }
        low = candidates.start;
        high = candidates.end;
      } else {
        high = len;
      }
    }
    const tie_break tie = route_tied(node, key, version, low, high, target);
    if (tie.node_changed) {
      return std::nullopt;
    }
    return routed{tie.edge, tie.at_separator, len};
  }

  // What the leaf's inline keys tell of `key`, read without a latch.
  template <class Q>
  [[gnu::always_inline]] static leaf_candidate candidate(leaf_node<T> leaf,
                                                         const search_key<T, Q>& key) noexcept {
    const racing_reads reads;
    const auto [len, prefix_len] = header(leaf.node(), leaf_node<T>::capacity);
    if constexpr (search_key<T, Q>::compares_inline) {
      const slot_range candidates = key_columns<T, true>(leaf.node().page())
                                        .inline_keys(len)
                                        .equal_range(key.inline_target(prefix_len).inline_key());
      if (candidates.empty()) {
        return {leaf_candidate::kind::absent, candidates.start, prefix_len};
      }
      if (candidates.size() == 1) {
        return {leaf_candidate::kind::one, candidates.start, prefix_len};
      }
    }
    return {leaf_candidate::kind::unknown, 0, prefix_len};
  }

  // Whether `key` may go after every key of `leaf`, read without a latch:
  // the leaf has a key, and its last key's inline key alone doesn't sort it
  // after `key`. A hint for an append, which latches the leaf to be sure
  // (D45): an insert that won't append latches nothing of the last leaf, as
  // every thread inserting at once would wait on it.
  template <class Q>
  static bool may_go_last(leaf_node<T> leaf, const search_key<T, Q>& key) noexcept {
    const racing_reads reads;
    const auto [len, prefix_len] = header(leaf.node(), leaf_node<T>::capacity);
    if (len == 0) {
      return false;
    }
    if constexpr (search_key<T, Q>::compares_inline) {
      return !key_columns<T, true>(leaf.node().page())
                  .inline_keys(len)
                  .is_above(len - 1, key.inline_target(prefix_len).inline_key());
    } else {
      return true;
    }
  }

  // The bytes of slot `index`'s key or value, copied without a latch.
  template <class U>
  static std::array<std::byte, sizeof(U)> copy_bytes(const U* slot) noexcept {
    const racing_reads reads;
    std::array<std::byte, sizeof(U)> bytes;
    std::memcpy(bytes.data(), slot, sizeof(U));
    return bytes;
  }

  // Hash tag `index` of `leaf`, read once without a latch; the leaf has
  // hash tags.
  static std::uint8_t tag(leaf_node<T> leaf, std::size_t index) noexcept {
    static_assert(T::leaf.hash_tags_offset.has_value());
    const racing_reads reads;
    const auto* tags =
        reinterpret_cast<const volatile std::uint8_t*>(leaf.node().page() + *T::leaf.hash_tags_offset);
    return tags[index < leaf_node<T>::capacity ? index : 0];
  }

  // Whether the leaf has a head, read once.
  static bool has_head(leaf_node<T> leaf) noexcept {
    const racing_reads reads;
    const auto* header = reinterpret_cast<const volatile node_header*>(leaf.node().page());
    return header->has_head;
  }

 private:
  // The leftmost leaf right of separator `index`, walked down first edges,
  // each step checked against `version`; none if a check failed.
  static std::optional<leaf_node<T>> head_leaf(internal_node<T> node, std::size_t index,
                                               latch_version version) noexcept {
    const node_latch latch(node.node().page());
    node_ptr<T> child = racing_node_reads::child(node, index + 1);
    while (true) {
      if (!latch.is_unchanged(version) || !child) {
        return std::nullopt;
      }
      if (child.is_leaf()) {
        return leaf_node<T>(child);
      }
      child = racing_node_reads::child(internal_node<T>(child), 0);
    }
  }

  // How `key` orders against separator `index`'s key, read at its head.
  template <class Q>
  static std::optional<std::weak_ordering> cmp_separator(internal_node<T> node, std::size_t index,
                                                         const search_key<T, Q>& key, latch_version version,
                                                         window target) noexcept {
    const std::optional<leaf_node<T>> head = head_leaf(node, index, version);
    if (!head) {
      return std::nullopt;
    }
    auto cmp = [&](const K& stored) {
      if constexpr (search_key<T, Q>::compares_inline) {
        return key.cmp_tied(*T::internal_plan.width, target, stored);
      } else {
        return T::order::cmp(key.key(), stored);
      }
    };
    const node_latch latch(node.node().page());
    if constexpr (orders_by_own_bytes) {
      const bool held = has_head(*head);
      const auto bytes = copy_bytes(head->head_key_slot().slot(0));
      if (!latch.is_unchanged(version) || !held) {
        return std::nullopt;
      }
      return cmp(std::bit_cast<K>(bytes));
    } else {
      node_latch head_latch(head->node().page());
      head_latch.lock_shared();
      std::optional<std::weak_ordering> ordering;
      if (latch.is_unchanged(version) && head->has_head()) {
        ordering = cmp(head->head_key());
      }
      head_latch.unlock_shared();
      return ordering;
    }
  }

  // What the tied separators' keys told: the edge the key takes and whether
  // it equals the separator left of that edge, or that a check failed. Two
  // words, which `route_tied` returns in registers. An optional of `routed`
  // returns through memory, and `route`, inlined, then kept its own result in
  // that memory too: every node's edge was stored and loaded back right
  // before the load of the child it names, on the one chain of dependent
  // loads a descent has.
  struct tie_break {
    std::size_t edge;
    bool at_separator;
    bool node_changed;
  };
  static_assert(sizeof(tie_break) <= 2 * sizeof(std::size_t));

  // Binary search over the tied separators, comparing their head keys.
  template <class Q>
  [[gnu::noinline]] static tie_break route_tied(internal_node<T> node, const search_key<T, Q>& key,
                                                latch_version version, std::size_t low, std::size_t high,
                                                window target) noexcept {
    while (low < high) {
      const std::size_t middle = low + (high - low) / 2;
      const std::optional<std::weak_ordering> ordering = cmp_separator(node, middle, key, version, target);
      if (!ordering) {
        return {0, false, true};
      }
      if (*ordering < 0) {
        high = middle;
      } else if (*ordering > 0) {
        low = middle + 1;
      } else {
        return {middle + 1, true, false};
      }
    }
    return {low, false, false};
  }
};

}  // namespace bbtree::detail
