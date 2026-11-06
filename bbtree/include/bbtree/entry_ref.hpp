// What a map's iterator dereferences to: a key and a value, by reference
// (PORTING.md, P7).
//
// A B+-tree keeps a separator's key apart from its value (D17, D49), so no
// `std::pair<const K, V>` exists in the nodes for `operator*` to return a
// reference to, as std::map's does. `entry_ref` is a pair of references
// instead, as `std::flat_map`'s is:
//
//   for (const auto& [key, value] : map)  // or auto&&, or auto
//   it->second = 4;                       // writes through
//
// `auto& [key, value]` does not compile: the reference is a value. The
// specializations of `std::basic_common_reference` below make the iterators
// model `std::bidirectional_iterator`, as std's `vector<bool>` and C++23's
// `zip` references do.
#pragma once

#include <type_traits>
#include <utility>

namespace bbtree {

template <class K, class V>
struct entry_ref {
  const K& first;
  V& second;

  using value_type = std::pair<const K, std::remove_const_t<V>>;

  operator value_type() const { return {first, second}; }  // NOLINT(google-explicit-constructor)
  operator std::pair<K, std::remove_const_t<V>>() const {  // NOLINT(google-explicit-constructor)
    return {first, second};
  }

  friend bool operator==(const entry_ref& a, const entry_ref& b) {
    return a.first == b.first && a.second == b.second;
  }
};

// What `iterator->` returns: the entry, held by value.
template <class K, class V>
class entry_arrow {
 public:
  explicit entry_arrow(entry_ref<K, V> entry) noexcept : entry_(entry) {}
  const entry_ref<K, V>* operator->() const noexcept { return &entry_; }

 private:
  entry_ref<K, V> entry_;
};

}  // namespace bbtree

// The common reference of an entry and the map's value_type is the
// value_type, which both convert to.
template <class K, class V, template <class> class TQual, template <class> class UQual>
struct std::basic_common_reference<bbtree::entry_ref<K, V>, std::pair<const K, std::remove_const_t<V>>, TQual,
                                   UQual> {
  using type = std::pair<const K, std::remove_const_t<V>>;
};

template <class K, class V, template <class> class TQual, template <class> class UQual>
struct std::basic_common_reference<std::pair<const K, std::remove_const_t<V>>, bbtree::entry_ref<K, V>, TQual,
                                   UQual> {
  using type = std::pair<const K, std::remove_const_t<V>>;
};
