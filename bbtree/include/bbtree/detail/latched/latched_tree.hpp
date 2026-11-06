// A tree that threads share (09-concurrency.md, D54): the unsync tree, with
// a latch word in each node, behind an atomic pointer to the root page.
//
// It is the unsync tree in every way but how threads share it: the same
// nodes, shared prefix (D50), prefix fits, splits, merges and appends. What
// it adds:
// - the descent, optimistic through the internal nodes; the leaf latched, or
//   for a lookup read at a version too (D57);
// - operations that run the caller's closure on an entry while its leaf is
//   latched, and let go before returning;
// - changes to more than a leaf, which take every node the unsync tree's
//   operation will change without waiting, then run that operation on the
//   tree as it is (`change_held`);
// - type-stable pages (`pages.hpp`).
#pragma once

#include <bbtree/detail/latched/latch.hpp>
#include <bbtree/detail/latched/optimistic.hpp>
#include <bbtree/detail/latched/pages.hpp>
#include <bbtree/detail/tree/tree.hpp>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>

namespace bbtree::detail {

// How a descent holds the leaf it ends at.
enum class leaf_mode : std::uint8_t { shared, exclusive, optimistic };

// A leaf held in `Mode` until destroyed: latched shared or exclusive, or read
// at a version (optimistic), which needs no letting go.
template <class T, leaf_mode Mode>
class held_leaf {
 public:
  held_leaf() noexcept = default;
  explicit held_leaf(leaf_node<T> leaf) noexcept : leaf_(leaf) {
    node_latch latch(leaf.node().page());
    if constexpr (Mode == leaf_mode::shared) {
      latch.lock_shared();
    } else if constexpr (Mode == leaf_mode::exclusive) {
      version_ = latch.lock_exclusive();
    } else {
      version_ = latch.read_version();
    }
    held_ = true;
  }
  held_leaf(held_leaf&& other) noexcept
      : leaf_(other.leaf_), version_(other.version_), held_(std::exchange(other.held_, false)) {}
  held_leaf& operator=(held_leaf&& other) noexcept {
    release();
    leaf_ = other.leaf_;
    version_ = other.version_;
    held_ = std::exchange(other.held_, false);
    return *this;
  }
  held_leaf(const held_leaf&) = delete;
  held_leaf& operator=(const held_leaf&) = delete;
  ~held_leaf() { release(); }

  leaf_node<T> leaf() const noexcept { return leaf_; }
  latch_version version() const noexcept { return version_; }
  bool is_unchanged() const noexcept {
    if constexpr (Mode == leaf_mode::optimistic) {
      return node_latch(leaf_.node().page()).is_unchanged(version_);
    } else {
      return true;
    }
  }

  void release() noexcept {
    if (!held_) {
      return;
    }
    held_ = false;
    node_latch latch(leaf_.node().page());
    if constexpr (Mode == leaf_mode::shared) {
      latch.unlock_shared();
    } else if constexpr (Mode == leaf_mode::exclusive) {
      latch.unlock_exclusive(version_);
    }
  }

 private:
  leaf_node<T> leaf_;
  latch_version version_;
  bool held_ = false;
};

// Where a descent ended: its leaf, held, and whether it matched a separator on
// the way, whose entry is the leaf's head (D17).
template <class T, leaf_mode Mode>
struct descent {
  held_leaf<T, Mode> leaf;
  bool at_separator;
};

template <class T>
class latched_tree {
  using K = typename T::key_type;
  using V = typename T::mapped_type;
  using reads = racing_node_reads<T>;

 public:
  latched_tree() noexcept = default;
  latched_tree(const latched_tree&) = delete;
  latched_tree& operator=(const latched_tree&) = delete;

  bool has_root() const noexcept { return root_.load(std::memory_order_acquire) != 0; }

  // Takes the tree apart and gives every page back: once no thread reaches it.
  template <class A>
  void destroy(const page_source<A>& source) noexcept {
    if (has_root()) {
      tree<T> whole = view();
      whole.destroy(source);
      root_.store(0, std::memory_order_relaxed);
      rightmost_leaf_.store(0, std::memory_order_relaxed);
      shared_ = shared_prefix();
    }
    free_.deallocate_all(source);
  }

  // Runs `operation` on the tree as an unsync tree, as if it owned the nodes
  // it changes: by an owner, or by a thread holding every node the operation
  // changes. Writes the shared prefix and the root pointer back.
  template <class Operation>
  decltype(auto) change_held(Operation&& operation) {
    tree<T> whole = view();
    const shared_prefix before = whole.shared();
    // The tree points appends at its last leaf, whichever it is (D45).
    struct write_back {
      latched_tree& self;
      tree<T>& whole;
      const shared_prefix& before;
      ~write_back() {
        if (!(whole.shared() == before)) {
          self.shared_ = whole.shared();
        }
        const node_ptr<T> root = whole.has_root() ? whole.root() : node_ptr<T>();
        self.rightmost_leaf_.store(
            root ? node_end_of<T>::template border_leaf<node_end::last>(root).node().tagged() : 0,
            std::memory_order_release);
        self.root_.store(root.tagged(), std::memory_order_release);
        (void)whole.release_root();
      }
    } done{*this, whole, before};
    return operation(whole);
  }

  // --- Lookups (D57) ---

  // Runs `f(key, value)` on the entry at `query` with its leaf latched shared.
  template <class Q, class F>
  auto read(const Q& query, F&& f) const {
    using result = closure_result<std::invoke_result_t<F&, const K&, const V&>>;
    if (!has_root()) {
      return result::none();
    }
    const search_key<T, Q> key(query);
    auto found = descend<leaf_mode::shared>(key);
    if (!found) {
      return result::none();
    }
    const leaf_node<T> leaf = found->leaf.leaf();
    if (found->at_separator) {
      return result::of(f, std::as_const(leaf.head_key()), std::as_const(leaf.head_value()));
    }
    const std::optional<std::size_t> index = leaf.find(key);
    if (!index) {
      return result::none();
    }
    return result::of(f, leaf.key(*index), std::as_const(leaf.value(*index)));
  }

  // A copy of the value at `query`: read unlatched where the leaf settles the
  // key without following a pointer, as a sequence lock reads (D57).
  template <class Q>
  std::optional<V> get_copied(const Q& query) const
    requires std::is_trivially_copyable_v<V>
  {
    if (!has_root()) {
      return std::nullopt;
    }
    const search_key<T, Q> key(query);
    if constexpr (reads::orders_by_own_bytes && search_key<T, Q>::compares_inline) {
      if (auto settled = find_unlatched(key)) {
        return *settled;
      }
    }
    return read(query, [](const K&, const V& value) { return value; });
  }

  template <class Q>
  bool contains(const Q& query) const {
    if (!has_root()) {
      return false;
    }
    const search_key<T, Q> key(query);
    auto found = descend<leaf_mode::optimistic>(key);
    if (!found) {
      return false;
    }
    if (!found->at_separator) {
      const leaf_candidate candidate = reads::candidate(found->leaf.leaf(), key);
      if (candidate.found == leaf_candidate::kind::absent && found->leaf.is_unchanged()) {
        return false;
      }
    }
    return read(query, [](const K&, const V&) {});
  }

  // Runs `f(key, value)` with the entry's leaf latched exclusive.
  template <class Q, class F>
  auto update(const Q& query, F&& f) {
    using result = closure_result<std::invoke_result_t<F&, const K&, V&>>;
    if (!has_root()) {
      return result::none();
    }
    const search_key<T, Q> key(query);
    auto found = descend<leaf_mode::exclusive>(key);
    if (!found) {
      return result::none();
    }
    const leaf_node<T> leaf = found->leaf.leaf();
    if (found->at_separator) {
      return result::of(f, std::as_const(leaf.head_key()), leaf.head_value());
    }
    const std::optional<std::size_t> index = leaf.find(key);
    if (!index) {
      return result::none();
    }
    return result::of(f, leaf.key(*index), leaf.value(*index));
  }

  // --- The descent ---

  // Descends to the leaf where `key` belongs, holding it in `Mode`; none for a
  // key outside what every key shares (D50). Restarts from the root on any
  // failed check.
  template <leaf_mode Mode, class Q>
  std::optional<descent<T, Mode>> descend(const search_key<T, Q>& key) const noexcept {
    while (true) {
      const node_ptr<T> root = root_node();
      if (root.is_leaf()) {
        held_leaf<T, Mode> held{leaf_node<T>(root)};
        if (root_node() != root) {
          continue;
        }
        std::optional<std::strong_ordering> outside;
        if constexpr (Mode == leaf_mode::optimistic) {
          outside = racing_shared().place_outside(key);
          if (!held.is_unchanged()) {
            continue;
          }
        } else {
          outside = shared_.place_outside(key);
        }
        if (outside) {
          return std::nullopt;
        }
        return descent<T, Mode>{std::move(held), false};
      }
      internal_node<T> node(root);
      latch_version version = node_latch(root.page()).read_version();
      if (root_node() != root) {
        continue;
      }
      if (racing_shared().place_outside(key)) {
        if (node_latch(root.page()).is_unchanged(version)) {
          return std::nullopt;
        }
        continue;
      }
      bool at_separator = false;
      bool restart = false;
      while (!restart) {
        const node_latch latch(node.node().page());
        const std::optional<routed> route = reads::route(node, key, version);
        if (!route) {
          break;
        }
        const node_ptr<T> child = reads::child(node, route->edge);
        if (!latch.is_unchanged(version) || !child) {
          break;
        }
        at_separator |= route->at_separator;
        if (child.is_leaf()) {
          held_leaf<T, Mode> held{leaf_node<T>(child)};
          if (!latch.is_unchanged(version)) {
            break;
          }
          if constexpr (Mode != leaf_mode::optimistic) {
            if (at_separator && !is_head(held.leaf(), key)) {
              break;
            }
          }
          return descent<T, Mode>{std::move(held), at_separator};
        }
        const latch_version child_version = node_latch(child.page()).read_version();
        if (!latch.is_unchanged(version)) {
          break;
        }
        node = internal_node<T>(child);
        version = child_version;
      }
    }
  }

  node_ptr<T> root_node() const noexcept {
    return node_ptr<T>::from_tagged(root_.load(std::memory_order_acquire));
  }

  // What every key shares, copied out while writers may change it.
  shared_prefix racing_shared() const noexcept {
    const racing_reads reads_guard;
    shared_prefix copy;
    std::memcpy(static_cast<void*>(&copy), static_cast<const void*>(&shared_), sizeof copy);
    return copy;
  }

  // Checks the tree's structure (`invariants.hpp`) and that appends go to
  // its last leaf; once no other thread changes it.
  void check_invariants(std::size_t length) {
    tree<T> whole = view();
    struct release_on_exit {
      tree<T>& whole;
      ~release_on_exit() { (void)whole.release_root(); }
    } release{whole};
    whole.forget_rightmost_leaf();
    whole.check_invariants(length);
    if (whole.has_root()) {
      const node_ptr<T> last = node_end_of<T>::template border_leaf<node_end::last>(whole.root()).node();
      if (rightmost_leaf_.load(std::memory_order_relaxed) != last.tagged()) {
        throw invariant_violation("appends go to a leaf that is not the last");
      }
    }
  }

 private:
  // The tree as an unsync tree over the same nodes, for an owner or a thread
  // holding what it changes; its root page goes back by `release_root`.
  tree<T> view() const noexcept {
    return tree<T>::of_root(root_page<T>(root_node()), shared_,
                            node_ptr<T>::from_tagged(rightmost_leaf_.load(std::memory_order_relaxed)));
  }

  template <class Q>
  static bool is_head(leaf_node<T> leaf, const search_key<T, Q>& key) noexcept {
    return leaf.has_head() && key.cmp_full(leaf.head_key()) == 0;
  }

  // The value at `key`, read and compared as a copy at the leaf's version,
  // where keys order by their own bytes; none where that doesn't settle it.
  template <class Q>
  std::optional<std::optional<V>> find_unlatched(const search_key<T, Q>& key) const noexcept {
    auto found = descend<leaf_mode::optimistic>(key);
    if (!found) {
      return std::optional<V>();
    }
    const leaf_node<T> leaf = found->leaf.leaf();
    if (found->at_separator) {
      const bool held = reads::has_head(leaf);
      const auto key_bytes = reads::copy_bytes(leaf.head_key_slot().slot(0));
      const auto value_bytes = reads::copy_bytes(leaf.head_value_address());
      if (!found->leaf.is_unchanged() || !held || key.cmp_full(std::bit_cast<K>(key_bytes)) != 0) {
        return std::nullopt;
      }
      return std::optional<V>(std::bit_cast<V>(value_bytes));
    }
    const leaf_candidate candidate = reads::candidate(leaf, key);
    if (candidate.found == leaf_candidate::kind::absent) {
      return found->leaf.is_unchanged() ? std::optional<std::optional<V>>(std::optional<V>()) : std::nullopt;
    }
    if (candidate.found == leaf_candidate::kind::unknown) {
      return std::nullopt;
    }
    const auto key_bytes = reads::copy_bytes(leaf.key_address(candidate.index));
    const auto value_bytes = reads::copy_bytes(leaf.value_address(candidate.index));
    if (!found->leaf.is_unchanged()) {
      return std::nullopt;
    }
    if (key.cmp_full(std::bit_cast<K>(key_bytes)) != 0) {
      return std::optional<V>();
    }
    return std::optional<V>(std::bit_cast<V>(value_bytes));
  }

  std::atomic<std::uintptr_t> root_{0};
  shared_prefix shared_;
  alignas(128) std::atomic<std::uintptr_t> rightmost_leaf_{0};
  alignas(128) mutable free_lists<T> free_;
};

}  // namespace bbtree::detail
