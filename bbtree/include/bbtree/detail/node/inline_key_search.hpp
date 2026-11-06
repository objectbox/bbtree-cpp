// A node's inline keys read in place for a search (D18), and its hash tags
// (D36). A search finds the inline keys equal to its own by counting over
// every slot up to the capacity, not by a binary search: the count's loads
// don't depend on one another, so a node's lines arrive together (04, Q6).
//
// The count runs as the compiler vectorizes it, but on x86-64 without POPCNT,
// where an SSE2 mask that fits one word gives both bounds by trailing-one
// counts (D40, D43). On x86-64 without SSE2 (kernels), a binary search.
// The Rust crate's explicit AVX-512 and NEON counts are not ported
// (PORTING.md, P9).
#pragma once

#include <bbtree/inline_key.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace bbtree::detail {

// The slot type a node keeps an inline key's first bytes in, and back.
template <inline_width W>
struct inline_slot {
  using type = std::conditional_t<W == inline_width::two, std::uint16_t,
                                  std::conditional_t<W == inline_width::four, std::uint32_t, std::uint64_t>>;
  static constexpr unsigned bits = 8 * sizeof(type);

  static type of(inline_word inline_key) noexcept { return static_cast<type>(inline_key >> (64 - bits)); }
  static inline_word inline_key(type slot) noexcept { return static_cast<inline_word>(slot) << (64 - bits); }
};

// The positions of the inline keys equal to a target: those below it, then
// those equal to it.
struct slot_range {
  std::size_t start;
  std::size_t end;

  constexpr bool empty() const noexcept { return start == end; }
  constexpr std::size_t size() const noexcept { return end - start; }
};

// A node's inline keys at width `W`: `Capacity` slots, the first `len` live,
// the rest holding the greatest inline key (invariant 4). The capacity is a
// constant, so the count unrolls.
template <inline_width W, std::size_t Capacity>
class node_inline_keys {
  using slot_traits = inline_slot<W>;
  using slot_type = typename slot_traits::type;

 public:
  node_inline_keys(const void* slots, std::size_t len) noexcept
      : slots_(static_cast<const slot_type*>(slots)), len_(len) {}

  std::size_t len() const noexcept { return len_; }

  inline_word inline_key(std::size_t index) const noexcept { return slot_traits::inline_key(load(index)); }

  // Whether inline key `index` is above the bytes the node keeps of
  // `inline_key`.
  bool is_above(std::size_t index, inline_word inline_key) const noexcept {
    return load(index) > slot_traits::of(inline_key);
  }

  // The positions of the inline keys equal to the bytes the node keeps of
  // `inline_key`: exactly those for sorted inline keys, a range within
  // 0..len for any; if none, the empty range where it belongs.
  [[gnu::always_inline]] slot_range equal_range(inline_word inline_key) const noexcept {
    const slot_type target = slot_traits::of(inline_key);
#if defined(__x86_64__) && !defined(__SSE2__)
    return equal_range_by_binary_search(target);
#else
#if defined(__SSE2__) && !defined(__POPCNT__)
    if constexpr (sse2_mask_fits) {
      return equal_range_by_sse2_mask(target);
    }
#endif
    return equal_range_by_count(target);
#endif
  }

  // Whether every slot past `len` holds the greatest inline key.
  bool pads_with_greatest() const noexcept {
    for (std::size_t index = len_; index < Capacity; ++index) {
      if (load(index) != static_cast<slot_type>(~slot_type{0})) {
        return false;
      }
    }
    return true;
  }

 private:
  slot_type load(std::size_t index) const noexcept {
    slot_type slot;
    std::memcpy(&slot, slots_ + index, sizeof slot);
    return slot;
  }

  // One branch-free pass over every slot counts those below the target and
  // those equal to it; slots past `len` hold the greatest inline key, so
  // they never count below, and the cuts at `len` keep the range in bounds.
  //
  // The counters have the slots' own width, which holds any count up to the
  // capacity: 64-bit counters would widen every compared lane, and the
  // compiler would count four slots a vector instead of sixteen.
  [[gnu::always_inline]] slot_range equal_range_by_count(slot_type target) const noexcept {
    static_assert(Capacity <= 0xffff, "a count fits the narrowest slot");
    slot_type below_count = 0;
    slot_type equal_count = 0;
    for (std::size_t index = 0; index < Capacity; ++index) {
      const slot_type slot = slots_[index];
      below_count = static_cast<slot_type>(below_count + (slot < target ? 1 : 0));
      equal_count = static_cast<slot_type>(equal_count + (slot == target ? 1 : 0));
    }
    const std::size_t below = below_count < len_ ? below_count : len_;
    const std::size_t end = below + equal_count < len_ ? below + equal_count : len_;
    return {below, end};
  }

  // The first live slot from `low` for which `is_before` is false, by a
  // branchless binary search.
  template <class IsBefore>
  std::size_t partition_point(std::size_t low, IsBefore is_before) const noexcept {
    std::size_t base = low;
    std::size_t size = len_ - low;
    if (size == 0) {
      return low;
    }
    while (size > 1) {
      const std::size_t half = size / 2;
      const std::size_t mid = base + half;
      base = is_before(load(mid)) ? mid : base;
      size -= half;
    }
    return base + (is_before(load(base)) ? 1 : 0);
  }

  slot_range equal_range_by_binary_search(slot_type target) const noexcept {
    const std::size_t start = partition_point(0, [target](slot_type slot) { return slot < target; });
    const std::size_t end = partition_point(start, [target](slot_type slot) { return slot <= target; });
    return {start, end};
  }

#if defined(__SSE2__)
  static constexpr std::size_t slot_bytes = sizeof(slot_type);
  static constexpr std::size_t lanes = 16 / slot_bytes;
  static constexpr std::size_t unpacked_max_slots = 64 / slot_bytes;
  // Above one word of byte masks, one packing stage halves each lane (D40).
  static constexpr bool packed = Capacity > unpacked_max_slots;
  static constexpr bool sse2_mask_fits = slot_bytes <= 4 && Capacity >= lanes &&
                                         Capacity <= (packed ? 2 * unpacked_max_slots : unpacked_max_slots) &&
                                         (!packed || Capacity >= 2 * lanes);

  static __m128i set1(slot_type value) noexcept {
    if constexpr (slot_bytes == 2) {
      return _mm_set1_epi16(static_cast<short>(value));
    } else {
      return _mm_set1_epi32(static_cast<int>(value));
    }
  }
  static __m128i greater(__m128i a, __m128i b) noexcept {
    if constexpr (slot_bytes == 2) {
      return _mm_cmpgt_epi16(a, b);
    } else {
      return _mm_cmpgt_epi32(a, b);
    }
  }
  static __m128i equal_lanes(__m128i a, __m128i b) noexcept {
    if constexpr (slot_bytes == 2) {
      return _mm_cmpeq_epi16(a, b);
    } else {
      return _mm_cmpeq_epi32(a, b);
    }
  }
  static __m128i pack(__m128i a, __m128i b) noexcept {
    if constexpr (slot_bytes == 2) {
      return _mm_packs_epi16(a, b);
    } else {
      return _mm_packs_epi32(a, b);
    }
  }
  __m128i load_lanes(std::size_t start) const noexcept {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(slots_ + start));
  }

  // SSE2 compares as signed, so both sides flip their sign bit. Sorted slots
  // make the `< target` and `<= target` masks runs of low ones; their first
  // zeros are the bounds, with no software population count.
  slot_range equal_range_by_sse2_mask(slot_type target) const noexcept {
    constexpr slot_type sign_bit = static_cast<slot_type>(slot_type{1} << (8 * slot_bytes - 1));
    const __m128i flip = set1(sign_bit);
    const __m128i flipped_target = set1(static_cast<slot_type>(target ^ sign_bit));
    const std::size_t bits_per_slot = packed ? slot_bytes / 2 : slot_bytes;
    std::uint64_t below_bits = 0;
    std::uint64_t through_bits = 0;
    if constexpr (packed) {
      for (std::size_t offset = 0; offset < Capacity; offset += 2 * lanes) {
        const std::size_t start = offset < Capacity - 2 * lanes ? offset : Capacity - 2 * lanes;
        const __m128i first = _mm_xor_si128(load_lanes(start), flip);
        const __m128i second = _mm_xor_si128(load_lanes(start + lanes), flip);
        const __m128i lower = pack(greater(flipped_target, first), greater(flipped_target, second));
        const __m128i above = pack(greater(first, flipped_target), greater(second, flipped_target));
        below_bits |= static_cast<std::uint64_t>(static_cast<unsigned>(_mm_movemask_epi8(lower)))
                      << (start * bits_per_slot);
        through_bits |= static_cast<std::uint64_t>(static_cast<std::uint16_t>(~_mm_movemask_epi8(above)))
                        << (start * bits_per_slot);
      }
    } else {
      for (std::size_t offset = 0; offset < Capacity; offset += lanes) {
        const std::size_t start = offset < Capacity - lanes ? offset : Capacity - lanes;
        const __m128i chunk = _mm_xor_si128(load_lanes(start), flip);
        const auto lower = static_cast<std::uint64_t>(
            static_cast<unsigned>(_mm_movemask_epi8(greater(flipped_target, chunk))));
        const auto equal = static_cast<std::uint64_t>(
            static_cast<unsigned>(_mm_movemask_epi8(equal_lanes(flipped_target, chunk))));
        below_bits |= lower << (start * slot_bytes);
        through_bits |= (lower | equal) << (start * slot_bytes);
      }
    }
    std::size_t below = static_cast<std::size_t>(std::countr_one(below_bits)) / bits_per_slot;
    std::size_t through = static_cast<std::size_t>(std::countr_one(through_bits)) / bits_per_slot;
    below = below < len_ ? below : len_;
    through = through < len_ ? through : len_;
    return {below, through};
  }
#endif

  const slot_type* slots_;
  std::size_t len_;
};

// Which of a lookup's candidates have its hash tag: none, one, two (at these
// positions), or more.
struct tag_matches {
  enum class count : std::uint8_t { none, one, two, many };
  count found;
  std::size_t lower;
  std::size_t upper;
};

// A leaf's hash tags (D36): a byte per key, unsorted; equal keys have equal
// tags, so a tag tells candidates apart, never orders them.
template <std::size_t Capacity>
class hash_tags {
 public:
  explicit hash_tags(const std::uint8_t* tags) noexcept : tags_(tags) {}

  // The tag of every byte string of 255 bytes or more.
  static constexpr std::uint8_t saturated_length = 255;

  // The positions among `candidates` whose tag is `target`.
  tag_matches matching(slot_range candidates, std::uint8_t target) const noexcept {
#if defined(__SSE2__)
    if constexpr (Capacity >= 16 && Capacity <= 64) {
      return matching_in_vectors(candidates, target);
    }
#endif
    std::size_t found[3];
    std::size_t count = 0;
    for (std::size_t index = candidates.start; index < candidates.end && count < 3; ++index) {
      if (tags_[index] == target) {
        found[count++] = index;
      }
    }
    switch (count) {
      case 0:
        return {tag_matches::count::none, 0, 0};
      case 1:
        return {tag_matches::count::one, found[0], 0};
      case 2:
        return {tag_matches::count::two, found[0], found[1]};
      default:
        return {tag_matches::count::many, 0, 0};
    }
  }

  std::uint8_t tag(std::size_t index) const noexcept { return tags_[index]; }

 private:
#if defined(__SSE2__)
  tag_matches matching_in_vectors(slot_range candidates, std::uint8_t target) const noexcept {
    constexpr std::size_t lanes = 16;
    std::uint64_t bits = 0;
    const __m128i wanted = _mm_set1_epi8(static_cast<char>(target));
    for (std::size_t offset = 0; offset < Capacity; offset += lanes) {
      const std::size_t start = offset < Capacity - lanes ? offset : Capacity - lanes;
      const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tags_ + start));
      const auto equal =
          static_cast<std::uint64_t>(static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, wanted))));
      bits |= equal << start;
    }
    const std::size_t start = candidates.start < Capacity ? candidates.start : Capacity;
    const std::size_t end = candidates.end < Capacity ? candidates.end : Capacity;
    const std::uint64_t from_start = start >= 64 ? 0 : ~std::uint64_t{0} << start;
    const std::uint64_t below_end = end == 0 ? 0 : ~std::uint64_t{0} >> (64 - end);
    bits &= from_start & below_end;
    if (bits == 0) {
      return {tag_matches::count::none, 0, 0};
    }
    const std::uint64_t remaining = bits & (bits - 1);
    const auto lowest = static_cast<std::size_t>(std::countr_zero(bits));
    if (remaining == 0) {
      return {tag_matches::count::one, lowest, 0};
    }
    if ((remaining & (remaining - 1)) == 0) {
      return {tag_matches::count::two, lowest, static_cast<std::size_t>(std::countr_zero(remaining))};
    }
    return {tag_matches::count::many, 0, 0};
  }
#endif

  const std::uint8_t* tags_;
};

}  // namespace bbtree::detail
