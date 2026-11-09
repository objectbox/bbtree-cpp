// The tree under the maps (D51): its root page, if it has one, which owns
// every node under it; what every key shares, the root's fences (D50); and
// the last leaf an append went to (D45). `node_stack_mut` is a node stack
// with exclusive access to a tree, which inserts, removals and refits climb.
//
// Every operation on a whole tree starts here. The operations live by
// feature: lookups below, prefix upkeep in `prefix.hpp`, inserts in
// `insert.hpp`, removals in `remove.hpp`, teardown in `teardown.hpp`, and
// the structural checks in `invariants.hpp`; this header includes them at
// its end.
#pragma once

#include <bbtree/detail/tree/node_stack.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace bbtree::detail {

template <class T>
class tree;

struct changed_fences;

// The pages an insert's split takes, from the map's allocator before the tree
// changes: an allocator that refuses one leaves the tree as it was, and the
// insert throws. The split then takes them as it goes.
template <class A>
class reserved_pages {
 public:
  explicit reserved_pages(const page_source<A>& source) noexcept : source_(source) {}
  reserved_pages(const reserved_pages&) = delete;
  reserved_pages& operator=(const reserved_pages&) = delete;
  ~reserved_pages() {
    for (std::size_t slot = 0; slot < count_; ++slot) {
      if (pages_[slot].page != nullptr) {
        source_.deallocate(pages_[slot].kind, pages_[slot].page, pages_[slot].layout);
      }
    }
  }

  // Takes `count` pages of `kind`; throws `alloc_error` if one is refused.
  void reserve(page_kind kind, page_layout layout, std::size_t count) {
    for (std::size_t taken = 0; taken < count; ++taken) {
      assert(count_ < max_pages);
      pages_[count_] = {source_.allocate(kind, layout), kind, layout};
      ++count_;
    }
  }

  void* allocate(page_kind kind, [[maybe_unused]] page_layout layout) const noexcept {
    for (std::size_t slot = 0; slot < count_; ++slot) {
      reserved& page = pages_[slot];
      if (page.page != nullptr && page.kind == kind) {
        assert(page.layout == layout && "a kind's pages have one layout");
        return std::exchange(page.page, nullptr);
      }
    }
    assert(false && "a page reserved for every page a split takes");
    return nullptr;
  }

  void deallocate(page_kind kind, void* page, page_layout layout) const noexcept {
    source_.deallocate(kind, page, layout);
  }

 private:
  static constexpr std::size_t max_pages = max_depth + 2;
  struct reserved {
    void* page;
    page_kind kind;
    page_layout layout;
  };

  const page_source<A>& source_;
  mutable reserved pages_[max_pages] = {};
  std::size_t count_ = 0;
};

// A node stack with exclusive access to its tree: the one kind that changes
// the tree, through its bottom node, and raises or lowers it at the root.
template <class T>
class node_stack_mut {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

 public:
  // Takes exclusive access, which ends the tree's permission to append to its
  // remembered leaf.
  explicit node_stack_mut(tree<T>& owner) noexcept;
  // At the position `at`, a node stack of `owner`.
  node_stack_mut(tree<T>& owner, const node_stack<T>& at) noexcept;

  tree<T>& owner() const noexcept { return tree_; }
  node_stack<T>& stack() noexcept { return stack_; }
  const node_stack<T>& stack() const noexcept { return stack_; }

  void start_at_root() noexcept;
  template <class Q>
  bool search(const search_key<T, Q>& key) noexcept;

  std::size_t depth() const noexcept { return stack_.depth(); }
  bool is_at_root() const noexcept { return stack_.is_at_root(); }
  std::size_t index() const noexcept { return stack_.index(); }
  void set_index(std::size_t index) noexcept { stack_.set_index(index); }
  node_ptr<T> node() const noexcept { return stack_.node(); }
  leaf_node<T> leaf() const noexcept { return stack_.leaf(); }
  internal_node<T> internal() const noexcept { return stack_.internal(); }
  internal_node<T> ascend() noexcept { return stack_.ascend(); }
  void descend() noexcept { stack_.descend(); }
  template <node_end End>
  void descend_to_leaf() noexcept {
    stack_.template descend_to_leaf<End>();
  }
  void detach() noexcept { stack_.detach(); }

  found_entry<T> found() const noexcept {
    assert(index() < node().len() && "the node stack ends at no key");
    return {node(), index()};
  }

  // Goes up a level, raising the tree first at the root, the one way inserts
  // grow it.
  template <class Pages>
  internal_node<T> ascend_raising(const Pages& pages);

  // The separators bounding the bottom node: its fences, if it has them.
  std::pair<const K*, const K*> fences() const noexcept;

  // Lowers a root left without separators; returns whether it did.
  template <class Pages>
  bool lower_an_emptied_root(bool fixed_root, const Pages& pages) noexcept;

  // Remembers the leaf the new entry went into if it went last everywhere.
  void finish_insertion() noexcept;

  // --- Inserting (`insert.hpp`) ---
  template <class KeyRef, class Pages>
  void insert_prefixed(const prefixed_key<KeyRef>& key, V&& value, const Pages& pages, std::size_t& length);

  // --- Removing (`remove.hpp`) ---
  template <class Pages>
  std::pair<K, V> remove(const Pages& pages, std::size_t& length) noexcept;
  // Removes the entry and ends at the next one, or detached past the last.
  template <class Pages>
  std::pair<K, V> remove_and_go_to_next(const Pages& pages, std::size_t& length) noexcept;
  // From a leaf: entry `slot - 1` for a slot within it, the separator left of
  // it for slot 0, the separator right of it for slot len + 1; detached if
  // there is none.
  void go_to_entry_at_slot(std::size_t slot) noexcept;

  // --- Borders and cuts (`borders.hpp`) ---
  template <node_end End>
  void refit_border_below(std::size_t edge) noexcept;
  void move_prefixes_back_to_root() noexcept;
  template <class Pages>
  node_ptr<T> cut(const Pages& pages);

  // --- Prefixes (`prefix.hpp`) ---
  void refit_split_halves(const std::size_t (&separator_indexes)[max_depth + 1],
                          std::size_t levels_climbed) noexcept;
  void refit_children_to_fences(std::size_t first_edge, std::optional<std::size_t> second_edge) noexcept;
  void refit_left_of_separator() noexcept;

 private:
  template <class KeyRef, class Pages>
  void insert_splitting(const prefixed_key<KeyRef>& key, V&& value, const Pages& pages, std::size_t& length);

  template <class Child>
  void refit_children_of_kind(std::size_t first_edge, std::optional<std::size_t> second_edge) noexcept;

  template <class Pages>
  std::pair<std::pair<K, V>, changed_fences> take_out(const Pages& pages, std::size_t& length) noexcept;
  template <class Pages, class AtSeparator>
  changed_fences fix_up_the_stack(std::size_t next_slot, std::optional<std::size_t> must_reach,
                                  const Pages& pages, AtSeparator&& at_separator) noexcept;
  template <class Pages>
  void lower_and_refit_changed_fences(changed_fences& changed, const Pages& pages) noexcept;

  tree<T>& tree_;
  node_stack<T> stack_;
};

// What an insert does with the value it was given when the tree holds its
// key: `try_emplace` keeps the entry's value, `insert_or_assign` assigns the
// new one to it. A template argument, not a flag, so that a mapped type that
// can't be assigned can still be inserted, as std::map's can.
enum class existing_value : std::uint8_t { kept, assigned };

// What an insert did: where the entry is, and whether it is new.
template <class T>
struct insertion {
  found_entry<T> entry;
  bool inserted;
};

template <class T>
class tree {
  using K = typename T::key_type;
  using V = typename T::mapped_type;

 public:
  tree() noexcept = default;
  tree(const tree&) = delete;
  tree& operator=(const tree&) = delete;
  tree(tree&& other) noexcept { *this = std::move(other); }
  tree& operator=(tree&& other) noexcept {
    root_ = std::exchange(other.root_, root_page<T>());
    rightmost_leaf_ = std::exchange(other.rightmost_leaf_, node_ptr<T>());
    shared_ = std::exchange(other.shared_, shared_prefix());
    return *this;
  }
  // No destructor: a tree leaks unless `destroy`ed with its allocator.

  // A tree over existing nodes: a latched tree's, for an owner or a thread
  // holding the nodes an operation changes (`latched_tree::change_held`).
  // `release_root` gives them back without taking them apart.
  static tree of_root(root_page<T> root, const shared_prefix& shared, node_ptr<T> rightmost) noexcept {
    tree view;
    view.root_ = root;
    view.shared_ = shared;
    view.rightmost_leaf_ = rightmost;
    return view;
  }
  root_page<T> release_root() noexcept {
    rightmost_leaf_ = node_ptr<T>();
    return std::exchange(root_, root_page<T>());
  }

  bool has_root() const noexcept { return static_cast<bool>(root_); }
  root_page<T> root_page_handle() const noexcept { return root_; }
  node_ptr<T> root() const noexcept {
    assert(has_root() && "an operation on a tree there is not");
    return root_.node();
  }
  const shared_prefix& shared() const noexcept { return shared_; }
  shared_prefix& shared_mut() noexcept { return shared_; }

  // Ends the permission to append to the remembered leaf, before a change
  // that could move or free it.
  void forget_rightmost_leaf() noexcept { rightmost_leaf_ = node_ptr<T>(); }
  node_ptr<T> remembered_rightmost_leaf() const noexcept { return rightmost_leaf_; }
  void remember_rightmost_leaf(leaf_node<T> leaf) noexcept { rightmost_leaf_ = leaf.node(); }

  // A new tree: an empty root leaf, nothing shared yet.
  template <class Pages>
  void make_root(const Pages& pages) {
    assert(!has_root());
    root_ = root_page<T>::create(fresh_page<T, page_role::root>::allocate(pages));
  }

  // The tree of `top` and the nodes under it, on plain pages, whose keys
  // share `shared`; `top` moves into a root page.
  template <class Pages>
  void adopt_top(node_ptr<T> top, const shared_prefix& shared, const Pages& pages) {
    auto [root, emptied] = root_page<T>::from_node(fresh_page<T, page_role::root>::allocate(pages), top);
    std::move(emptied).deallocate(pages);
    root_ = root;
    rightmost_leaf_ = node_ptr<T>();
    shared_ = shared;
  }

  template <class Pages>
  void raise(const Pages& pages) {
    forget_rightmost_leaf();
    root_ = root_.raise(pages, shared_.len());
  }

  any_page<T> lower() noexcept {
    forget_rightmost_leaf();
    auto [root, emptied] = root_.lower();
    root_ = root;
    return std::move(emptied);
  }

  // How many edges lead down from the root to each leaf.
  std::size_t leaf_depth() const noexcept {
    std::size_t depth = 0;
    for (node_ptr<T> node = root(); !node.is_leaf(); node = internal_node<T>(node).edge(node.len())) {
      ++depth;
    }
    return depth;
  }

  // --- Lookups ---

  template <class Q>
  std::optional<found_entry<T>> find(const Q& query) const noexcept {
    if (!has_root()) {
      return std::nullopt;
    }
    const search_key<T, Q> key(query);
    if (shared_.place_outside(key)) {
      return std::nullopt;
    }
    node_ptr<T> node = root();
    while (!node.is_leaf()) {
      const internal_node<T> internal(node);
      const search_result found = internal.search(key);
      if (found.found) {
        return found_entry<T>{node, found.index};
      }
      node = internal.edge(found.index);
    }
    if (const std::optional<std::size_t> index = leaf_node<T>(node).find(key)) {
      return found_entry<T>{node, *index};
    }
    return std::nullopt;
  }

  std::optional<found_entry<T>> first() const noexcept {
    if (!has_root()) {
      return std::nullopt;
    }
    const leaf_node<T> leaf = node_end_of<T>::template border_leaf<node_end::first>(root());
    return leaf.len() > 0 ? std::optional<found_entry<T>>(found_entry<T>{leaf.node(), 0}) : std::nullopt;
  }

  std::optional<found_entry<T>> last() const noexcept {
    if (!has_root()) {
      return std::nullopt;
    }
    const leaf_node<T> leaf = node_end_of<T>::template border_leaf<node_end::last>(root());
    return leaf.len() > 0 ? std::optional<found_entry<T>>(found_entry<T>{leaf.node(), leaf.len() - 1})
                          : std::nullopt;
  }

  // --- Inserting (`insert.hpp`) ---

  // Inserts `key` with the value `make_value()` makes, unless the tree holds
  // the key; then `Existing` says whether the made value is assigned to its
  // entry. The key (`const K&` or `K&&`) is copied or moved only if it goes
  // in. The value is made before the tree changes, and a split's pages are
  // taken before it: an exception from either leaves the tree's entries as
  // they were.
  template <existing_value Existing, class KeyRef, class A, class MakeValue>
  insertion<T> insert(KeyRef&& key, MakeValue&& make_value, const page_source<A>& pages, std::size_t& length);

  // --- Removing (`remove.hpp`) ---

  template <class Q, class Pages>
  std::optional<std::pair<K, V>> remove(const Q& query, const Pages& pages, std::size_t& length) noexcept;
  template <class Pages>
  std::optional<std::pair<K, V>> pop_first(const Pages& pages, std::size_t& length) noexcept;
  template <class Pages>
  std::optional<std::pair<K, V>> pop_last(const Pages& pages, std::size_t& length) noexcept;

  // --- Prefixes (`prefix.hpp`) ---

  // Shrinks what every key shares to what `key` shares too, before the tree
  // takes it; an empty tree takes `key`'s first bytes.
  template <class KeyRef>
  prefixed_key<KeyRef> shrink_shared_prefix_for(KeyRef key) noexcept;

  // --- Borders and cuts (`borders.hpp`) ---

  template <node_end End, class Pages>
  void fix_border(const Pages& pages) noexcept;
  template <class Pages>
  void lower_while_root_empty(const Pages& pages) noexcept;
  template <node_end End>
  void refit_border_siblings() noexcept;
  template <class Q, class Pages>
  std::pair<tree, std::size_t> split_off_at(const Q& query, key_side side, const Pages& pages,
                                            std::size_t& length);

  // --- Copies (`clone.hpp`) ---

  // A copy, node by node, of the same shape; copies keys and values, calls
  // no comparator and no inline-key encoding.
  template <class Pages>
  tree clone(const Pages& pages) const;

  // --- Teardown (`teardown.hpp`) ---

  // Drops every entry and gives every page back; the tree is empty after.
  template <class Pages>
  void destroy(const Pages& pages) noexcept;

  // --- Checks (`invariants.hpp`) ---

  void check_invariants(std::size_t length) const;

 private:
  template <existing_value Existing, class KeyRef, class A, class MakeValue>
  insertion<T> insert_with_stack(const prefixed_key<KeyRef>& key, MakeValue&& make_value,
                                 const page_source<A>& pages, std::size_t& length);

  // The entry of a key the tree holds already, as an insert leaves it.
  template <existing_value Existing, class MakeValue>
  static insertion<T> settle_existing(found_entry<T> entry, MakeValue& make_value) {
    if constexpr (Existing == existing_value::assigned) {
      *entry.value() = make_value();
    }
    return {entry, false};
  }

  // The append of `key` to the remembered last leaf, if it has room and
  // `key` is greater than its last key and shares what every key does.
  bool appends_to_rightmost(const K& key) noexcept;

  void shrink_borders_to(std::size_t len) noexcept;

  root_page<T> root_;
  node_ptr<T> rightmost_leaf_;
  shared_prefix shared_;
};

// --- node_stack_mut members that need the tree ---

template <class T>
node_stack_mut<T>::node_stack_mut(tree<T>& owner) noexcept : tree_(owner) {
  owner.forget_rightmost_leaf();
}

template <class T>
node_stack_mut<T>::node_stack_mut(tree<T>& owner, const node_stack<T>& at) noexcept
    : tree_(owner), stack_(at) {
  owner.forget_rightmost_leaf();
}

template <class T>
void node_stack_mut<T>::start_at_root() noexcept {
  stack_.attach(tree_.root());
}

template <class T>
template <class Q>
bool node_stack_mut<T>::search(const search_key<T, Q>& key) noexcept {
  start_at_root();
  return stack_.search(key, tree_.shared());
}

template <class T>
template <class Pages>
internal_node<T> node_stack_mut<T>::ascend_raising(const Pages& pages) {
  if (!stack_.is_at_root()) {
    return stack_.ascend();
  }
  tree_.raise(pages);
  stack_.attach(tree_.root());
  return stack_.internal_at(0);
}

template <class T>
std::pair<const typename T::key_type*, const typename T::key_type*> node_stack_mut<T>::fences()
    const noexcept {
  const K* low = nullptr;
  const K* high = nullptr;
  for (std::size_t at = stack_.depth(); at-- > 0;) {
    const internal_node<T> node = stack_.internal_at(at);
    const std::size_t edge = stack_.index_at(at);
    if (low == nullptr && edge > 0) {
      low = &node.separator_key(edge - 1);
    }
    if (high == nullptr && edge < node.len()) {
      high = &node.separator_key(edge);
    }
    if (low != nullptr && high != nullptr) {
      break;
    }
  }
  return {low, high};
}

template <class T>
template <class Pages>
bool node_stack_mut<T>::lower_an_emptied_root(bool fixed_root, const Pages& pages) noexcept {
  const node_ptr<T> root = tree_.root();
  const bool lowers = fixed_root && !root.is_leaf() && root.len() == 0;
  if (lowers) {
    stack_.detach();
    tree_.lower().deallocate(pages);
    start_at_root();
  }
  return lowers;
}

template <class T>
void node_stack_mut<T>::finish_insertion() noexcept {
  const leaf_node<T> bottom = leaf();
  if (stack_.index() + 1 != bottom.len()) {
    return;
  }
  for (std::size_t at = 0; at < stack_.depth(); ++at) {
    if (stack_.index_at(at) != stack_.internal_at(at).len()) {
      return;
    }
  }
  tree_.remember_rightmost_leaf(bottom);
}

}  // namespace bbtree::detail

#include <bbtree/detail/tree/borders.hpp>
#include <bbtree/detail/tree/build.hpp>
#include <bbtree/detail/tree/clone.hpp>
#include <bbtree/detail/tree/insert.hpp>
#include <bbtree/detail/tree/invariants.hpp>
#include <bbtree/detail/tree/prefix.hpp>
#include <bbtree/detail/tree/remove.hpp>
#include <bbtree/detail/tree/teardown.hpp>
