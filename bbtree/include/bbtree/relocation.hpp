// Whether a type moves as its bytes (PORTING.md, P6).
//
// A node shifts, splits and merges its keys and values constantly. The Rust
// crate moves every value as bytes, since every Rust move is a byte copy. In
// C++ only some types allow that: copying a trivially relocatable object's
// bytes to new storage, and forgetting the old storage, is a move. bbtree
// moves those with `memmove`, and every other type one object at a time, by
// move construction and destruction.
//
// Trivially copyable types are trivially relocatable. Specialize
// `bbtree::is_trivially_relocatable` for a type of yours that is too: one
// that holds no pointer into itself and keeps no registry of its address.
#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace bbtree {

template <class T>
struct is_trivially_relocatable : std::bool_constant<std::is_trivially_copyable_v<T>> {};

// Std types known to hold no pointer into themselves.
template <class T>
struct is_trivially_relocatable<std::unique_ptr<T>> : std::true_type {};

template <class T>
struct is_trivially_relocatable<std::shared_ptr<T>> : std::true_type {};

template <class T>
struct is_trivially_relocatable<std::vector<T>> : std::true_type {};

#if defined(_LIBCPP_VERSION)
// libc++'s short string is its bytes, with no pointer to them; libstdc++'s
// points into itself, so it is not trivially relocatable.
template <>
struct is_trivially_relocatable<std::string> : std::true_type {};
#endif

template <class T>
inline constexpr bool is_trivially_relocatable_v = is_trivially_relocatable<T>::value;

}  // namespace bbtree
