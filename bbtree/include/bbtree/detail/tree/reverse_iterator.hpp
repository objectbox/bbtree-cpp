// The maps' reverse iterators, what `rbegin()` and `rend()` return.
//
// `std::reverse_iterator` holds the iterator after its entry, so each
// dereference copies that iterator and steps the copy back. A map's iterator
// carries a node stack (`cursor.hpp`): the copy is 288 bytes, and a step back
// that crosses a leaf is done for the dereference and again for the step.
// And `rend()` would be `begin()`, a descent to the first leaf for every
// `it != map.rend()`.
//
// This one holds an iterator at its own entry. Dereferencing reads the
// position, a step is one step of the cursor the other way, and the reverse
// end is the position past the last entry, which a walk backwards from the
// first entry reaches too: `rend()` descends nowhere. `base()`, the iterator
// after the entry, is made when asked for.
#pragma once

#include <concepts>
#include <iterator>

namespace bbtree::detail {

template <class Iterator>
class reverse_iterator {
 public:
  using iterator_type = Iterator;
  using iterator_category = typename Iterator::iterator_category;
  using iterator_concept = typename Iterator::iterator_concept;
  using value_type = typename Iterator::value_type;
  using difference_type = typename Iterator::difference_type;
  using reference = typename Iterator::reference;
  using pointer = typename Iterator::pointer;

  reverse_iterator() noexcept = default;

  // At the entry before `base`, as `std::reverse_iterator(base)`; the reverse
  // end for a map's `begin()`.
  explicit reverse_iterator(const Iterator& base) noexcept : at_(base) { at_.cursor_.prev_or_end(); }

  // At `entry`'s own entry; a map's reverse end for its `end()`, the same
  // position.
  static reverse_iterator at(const Iterator& entry) noexcept {
    return reverse_iterator(entry, at_its_entry{});
  }

  // A reverse iterator converts to a const one, as its iterator does.
  template <class Other>
    requires(!std::same_as<Other, Iterator> && std::convertible_to<const Other&, Iterator>)
  reverse_iterator(const reverse_iterator<Other>& other) noexcept  // NOLINT(google-explicit-constructor)
      : at_(other.at_) {}

  // The iterator after this one's entry; a map's `begin()` at its reverse
  // end.
  Iterator base() const noexcept {
    Iterator after = at_;
    after.cursor_.next_or_first();
    return after;
  }

  reference operator*() const noexcept { return *at_; }
  pointer operator->() const noexcept { return at_.operator->(); }

  // Inlined into the loop that steps, as the iterators' steps are.
  [[gnu::always_inline]] reverse_iterator& operator++() noexcept {
    at_.cursor_.prev_or_end();
    return *this;
  }
  [[gnu::always_inline]] reverse_iterator operator++(int) noexcept {
    reverse_iterator before = *this;
    at_.cursor_.prev_or_end();
    return before;
  }
  reverse_iterator& operator--() noexcept {
    at_.cursor_.next_or_first();
    return *this;
  }
  reverse_iterator operator--(int) noexcept {
    reverse_iterator before = *this;
    at_.cursor_.next_or_first();
    return before;
  }

  friend bool operator==(const reverse_iterator& a, const reverse_iterator& b) noexcept {
    return a.at_ == b.at_;
  }

 private:
  template <class>
  friend class reverse_iterator;

  struct at_its_entry {};
  reverse_iterator(const Iterator& entry, at_its_entry /*unstepped*/) noexcept : at_(entry) {}

  // At this iterator's entry; a map's `end()` at the reverse end.
  Iterator at_;
};

}  // namespace bbtree::detail
