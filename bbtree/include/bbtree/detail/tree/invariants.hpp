// Checks that a tree is well formed (`tree::check_invariants`), through the
// node API: its shape, order and counts, and on every node, that its inline
// keys and hash tags match its keys and its prefix length is what its fences
// share, or along the borders, what every key shares (D50). Compiled where
// `BBTREE_CHECKED` is set: tests and fuzz targets.
#pragma once

#include <bbtree/detail/tree/tree.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace bbtree::detail {

// A broken tree invariant, which the checks throw.
class invariant_violation : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

template <class T>
class tree_checker {
  using K = typename T::key_type;
  using order = typename T::order;

 public:
  tree_checker(const tree<T>& checked, std::size_t length) noexcept : tree_(checked), length_(length) {}

  void run() {
    if (const node_ptr<T> remembered = tree_.remembered_rightmost_leaf()) {
      require(tree_.has_root(), "a remembered leaf without a tree");
      require(remembered == node_end_of<T>::template border_leaf<node_end::last>(tree_.root()).node(),
              "the remembered leaf is not the tree's last leaf");
    }
    if (!tree_.has_root()) {
      require(length_ == 0, "a map without a tree has entries");
      return;
    }
    const node_ptr<T> root = tree_.root();
    if (length_ > 0 || !root.is_leaf()) {
      require(root.len() > 0, "a root that is internal or holds a non-empty map's entries has no keys");
    }
    check_under(root, nullptr, nullptr, 0, tree_.shared().len(), true, true);
    require(separators_ + 1 == leaves_, "L leaves need L - 1 separators");
    require(entries_ == length_, "the length is not the entry count");
  }

 private:
  static void require(bool condition, const char* what) {
    if (!condition) {
      throw invariant_violation(what);
    }
  }

  void check_under(node_ptr<T> node, const K* lower, const K* upper, std::size_t depth,
                   std::size_t parent_prefix, bool on_left_border, bool on_right_border) {
    const std::size_t prefix_len = node.prefix_len();
    require(prefix_len >= parent_prefix, "a child's prefix is shorter than its parent's (D44)");
    require(prefix_len == tree_.shared().len() || !(on_left_border || on_right_border),
            "a border node's prefix is not what every key shares (D50)");
    if (node.is_leaf()) {
      const leaf_node<T> leaf(node);
      check_node(leaf, lower, upper, depth);
      if (!leaf_depth_) {
        leaf_depth_ = depth;
      }
      require(*leaf_depth_ == depth, "leaves at different depths");
      require(leaf.has_head() == (leaves_ > 0), "every leaf but the leftmost has a head, and only those");
      ++leaves_;
      for (std::size_t index = 0; index < leaf.len(); ++index) {
        visit(leaf.key(index));
      }
      return;
    }
    const internal_node<T> internal(node);
    check_node(internal, lower, upper, depth);
    for (std::size_t edge = 0; edge <= internal.len(); ++edge) {
      const K* child_lower = edge > 0 ? &internal.separator_key(edge - 1) : lower;
      const K* child_upper = edge < internal.len() ? &internal.separator_key(edge) : upper;
      require(edge == 0 || internal.edge(edge).is_leaf() == internal.edge(0).is_leaf(),
              "an internal node's children are of one kind");
      check_under(internal.edge(edge), child_lower, child_upper, depth + 1, prefix_len,
                  on_left_border && edge == 0, on_right_border && edge == internal.len());
      if (edge < internal.len()) {
        ++separators_;
        visit(internal.separator_key(edge));
      }
    }
  }

  template <class Node>
  void check_node(const Node& node, const K* lower, const K* upper, std::size_t depth) {
    if (depth > 0) {
      require(node.len() >= Node::min_len, "a node below the root has fewer keys than its minimum");
    }
    const std::size_t prefix_len = node.prefix_len();
    if constexpr (Node::columns_type::has_inline_keys) {
      constexpr unsigned dropped_bits = 8 * (8 - static_cast<unsigned>(*Node::plan.width));
      const auto inline_keys = node.inline_keys();
      require(inline_keys.pads_with_greatest(), "an inline-key slot past the length is not the greatest");
      for (std::size_t index = 0; index < node.len(); ++index) {
        const inline_word expected =
            T::inliner::inline_key_at(node.key(index), prefix_len) >> dropped_bits << dropped_bits;
        require(inline_keys.inline_key(index) == expected,
                "the inline keys are not the keys' windows at the node's prefix length");
      }
      if constexpr (Node::columns_type::has_hash_tags) {
        const auto tags = typename Node::columns_type(node.node().page()).tags();
        for (std::size_t index = 0; index < node.len(); ++index) {
          require(tags.tag(index) == T::inliner::hash_tag(node.key(index)), "a hash tag is not its key's");
        }
      }
      const std::size_t fence_prefix = lower != nullptr && upper != nullptr
                                           ? T::inliner::common_prefix_len(*lower, *upper)
                                           : tree_.shared().len();
      require(prefix_len == fence_prefix, "a node's prefix length is not what its fences share");
    } else {
      require(prefix_len == 0, "a node without inline keys has a prefix");
    }
    for (std::size_t index = 0; index < node.len(); ++index) {
      const K& key = node.key(index);
      require(lower == nullptr || order::less(*lower, key), "a key is not above its left separator");
      require(upper == nullptr || order::less(key, *upper), "a key is not below its right separator");
      require(tree_.shared().template is_shared_by<T>(key),
              "a key lacks what the tree says every key shares");
    }
  }

  void visit(const K& key) {
    require(previous_ == nullptr || order::less(*previous_, key), "keys do not strictly increase in order");
    previous_ = &key;
    ++entries_;
  }

  const tree<T>& tree_;
  std::size_t length_;
  const K* previous_ = nullptr;
  std::size_t entries_ = 0;
  std::size_t leaves_ = 0;
  std::size_t separators_ = 0;
  std::optional<std::size_t> leaf_depth_;
};

template <class T>
void tree<T>::check_invariants(std::size_t length) const {
  tree_checker<T>(*this, length).run();
}

}  // namespace bbtree::detail
