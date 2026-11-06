// `slots<T>`: a fixed-capacity array of slots inside a node page, side by side
// or a stride apart. Apart from whole pages, this is the only code that moves
// keys, values and edges within and between nodes (D9, D12, D14, D29).
//
// A `slots` neither owns its slots nor tracks a length: which slots hold an
// object is the caller's business, as each operation states.
//
// Moves relocate: a trivially relocatable type (`relocation.hpp`) moves as
// bytes, side-by-side runs in one `memmove`; any other type one object at a
// time, by move construction and destruction (PORTING.md, P6).
#pragma once

#include <bbtree/relocation.hpp>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace bbtree::detail {

template <class T>
class slots {
 public:
  // Empty types take no bytes (a set's values): every slot is one address.
  static constexpr bool takes_no_room = std::is_empty_v<T>;
  static constexpr bool moves_as_bytes = is_trivially_relocatable_v<T>;
  static_assert(!takes_no_room ||
                    (std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>),
                "an empty slot type must be trivial");

  constexpr slots() noexcept = default;
  slots(void* anchor, std::size_t stride) noexcept
      : anchor_(static_cast<std::byte*>(anchor)), stride_(stride) {}

  std::size_t stride() const noexcept { return stride_; }

  T* slot(std::size_t index) const noexcept {
    return std::launder(reinterpret_cast<T*>(anchor_ + index * stride_));
  }

  // A copy of slot `index`, read without forming a reference: what a shared
  // map's optimistic reader takes of a node a writer may change.
  T load(std::size_t index) const noexcept
    requires std::is_trivially_copyable_v<T>
  {
    T value;
    std::memcpy(&value, anchor_ + index * stride_, sizeof(T));
    return value;
  }

  // Constructs `value` in the empty slot `index`.
  void construct(std::size_t index, T&& value) const noexcept {
    if constexpr (!takes_no_room) {
      ::new (static_cast<void*>(anchor_ + index * stride_)) T(std::move(value));
    }
  }

  // Moves slot `index`'s object out, leaving the slot empty.
  T take(std::size_t index) const noexcept {
    if constexpr (takes_no_room) {
      return T{};
    } else {
      T* object = slot(index);
      T value(std::move(*object));
      object->~T();
      return value;
    }
  }

  // Replaces slot `index`'s object with `value` and returns the old one.
  T replace(std::size_t index, T&& value) const noexcept {
    T old = take(index);
    construct(index, std::move(value));
    return old;
  }

  void destroy(std::size_t index) const noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      slot(index)->~T();
    }
  }

  // Writes `value` into slot `index`, first shifting slots index..len up by
  // one into index + 1..len + 1. Slot `len` is empty before.
  void insert(std::size_t index, std::size_t len, T&& value) const noexcept {
    shift_up(index, len);
    construct(index, std::move(value));
  }

  // Takes slot `index` out, then shifts slots index + 1..len down by one into
  // index..len - 1. Slot len - 1 is empty after.
  T remove(std::size_t index, std::size_t len) const noexcept {
    T removed = take(index);
    shift_down(index, len);
    return removed;
  }

  // Moves slots index..len up by one; slot `len` is empty before, slot
  // `index` after.
  void shift_up(std::size_t index, std::size_t len) const noexcept {
    if constexpr (takes_no_room) {
      return;
    } else if constexpr (moves_as_bytes) {
      if (index == len) {
        return;
      }
      if (stride_ == sizeof(T)) {
        std::memmove(anchor_ + (index + 1) * stride_, anchor_ + index * stride_, (len - index) * sizeof(T));
      } else {
        for (std::size_t from = len; from-- > index;) {
          std::memcpy(anchor_ + (from + 1) * stride_, anchor_ + from * stride_, sizeof(T));
        }
      }
    } else {
      for (std::size_t from = len; from-- > index;) {
        relocate(from, *this, from + 1);
      }
    }
  }

  // Moves slots index + 1..len down by one; slot `index` is empty before,
  // slot len - 1 after.
  void shift_down(std::size_t index, std::size_t len) const noexcept {
    if constexpr (takes_no_room) {
      return;
    } else if constexpr (moves_as_bytes) {
      if (index + 1 >= len) {
        return;
      }
      if (stride_ == sizeof(T)) {
        std::memmove(anchor_ + index * stride_, anchor_ + (index + 1) * stride_,
                     (len - index - 1) * sizeof(T));
      } else {
        for (std::size_t from = index + 1; from < len; ++from) {
          std::memcpy(anchor_ + (from - 1) * stride_, anchor_ + from * stride_, sizeof(T));
        }
      }
    } else {
      for (std::size_t from = index + 1; from < len; ++from) {
        relocate(from, *this, from - 1);
      }
    }
  }

  // Moves slots start..start + count into dest's dest_start..dest_start +
  // count, keeping their order; the runs don't overlap, and the destination
  // slots are empty before, the source slots after.
  void move_run_to(std::size_t start, std::size_t count, slots dest, std::size_t dest_start) const noexcept {
    if constexpr (takes_no_room) {
      return;
    } else if constexpr (moves_as_bytes) {
      if (stride_ == sizeof(T) && dest.stride_ == sizeof(T)) {
        if (count > 0) {
          std::memcpy(dest.anchor_ + dest_start * sizeof(T), anchor_ + start * sizeof(T), count * sizeof(T));
        }
      } else {
        for (std::size_t offset = 0; offset < count; ++offset) {
          std::memcpy(dest.anchor_ + (dest_start + offset) * dest.stride_,
                      anchor_ + (start + offset) * stride_, sizeof(T));
        }
      }
    } else {
      for (std::size_t offset = 0; offset < count; ++offset) {
        relocate(start + offset, dest, dest_start + offset);
      }
    }
  }

  // Sets every byte of `count` side-by-side slots to `byte`: for plain data.
  void fill_bytes(std::size_t count, unsigned char byte) const noexcept
    requires std::is_trivially_copyable_v<T>
  {
    std::memset(anchor_, byte, count * sizeof(T));
  }

  const std::byte* address() const noexcept { return anchor_; }

 private:
  // Moves slot `index`'s object into `dest`'s empty slot `dest_index`, one
  // move and one destruction, leaving slot `index` empty.
  void relocate(std::size_t index, slots dest, std::size_t dest_index) const noexcept {
    T* object = slot(index);
    ::new (static_cast<void*>(dest.anchor_ + dest_index * dest.stride_)) T(std::move(*object));
    object->~T();
  }

  std::byte* anchor_ = nullptr;
  std::size_t stride_ = 0;
};

// Shifts whole entries, the strides of slots index..len, up by one in one
// copy, with the fields between them: how a leaf of paired entries inserts
// when both its keys and values move as bytes.
struct entry_shift {
  static void up(std::byte* first_entry, std::size_t stride, std::size_t index, std::size_t len) noexcept {
    if (index < len) {
      std::memmove(first_entry + (index + 1) * stride, first_entry + index * stride, (len - index) * stride);
    }
  }
  static void down(std::byte* first_entry, std::size_t stride, std::size_t index, std::size_t len) noexcept {
    if (index + 1 < len) {
      std::memmove(first_entry + index * stride, first_entry + (index + 1) * stride,
                   (len - index - 1) * stride);
    }
  }
};

}  // namespace bbtree::detail
