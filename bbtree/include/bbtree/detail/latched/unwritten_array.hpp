// Scratch arrays that a change writes before it reads (PORTING.md, P37).
#pragma once

#include <cstddef>
#include <type_traits>

namespace bbtree::detail {

// `N` objects of `T`, left unwritten until assigned: the shared map's
// changes record their path, the nodes they hold and the pages they take in
// arrays of the deepest tree's size, of which a change uses a few entries,
// and zeroing them (their element types zero themselves when
// default-constructed) cost every split and every merging removal about
// 2 KiB of stores. An element's life begins when it is assigned, which the
// union allows for trivially copyable types; reading one not yet assigned
// is a bug, as reading past a count would be.
template <class T, std::size_t N>
class unwritten_array {
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);

 public:
  // Writes nothing.
  unwritten_array() noexcept {}

  T& operator[](std::size_t index) noexcept { return items_[index]; }
  const T& operator[](std::size_t index) const noexcept { return items_[index]; }
  const T* data() const noexcept { return items_; }

 private:
  union {
    T items_[N];
  };
};

}  // namespace bbtree::detail
