// The count over a node's inline keys (`inline_key_search.hpp`) in explicit
// x86-64 vectors, for every slot width and capacity. Written with
// intrinsics, not left to the compiler's vectorizer: GCC at -O2 doesn't
// vectorize such a loop, and counts a slot at a time with a branch each.
//
// Every slot up to the capacity is compared with the target, once for
// `slot < target` and once for `slot > target`. The compares' lanes are
// packed down to bytes, and a register of bytes gives a mask, a bit per
// byte. What reduces the masks to counts depends on the register:
//
// - SSE2's packs keep the lanes' order, so its masks have the slots' order.
//   Sorted slots make them runs of low bits, whose lengths are the counts
//   (D40, D43): no population count, which a baseline target has no
//   instruction for.
// - AVX2's packs work in each half of a register, so its masks are out of
//   order, and are counted by population count, which every AVX2 CPU has.
//
// A capacity that is no multiple of a register's lanes ends in a vector of
// its last slots, which repeats some of the vector before; their compares
// are dropped, so no load leaves the slots and no slot counts twice.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace bbtree::detail {

// How many of a node's slots, up to its capacity, are below a target, and
// how many are not above it.
struct slot_counts {
  std::size_t below;
  std::size_t through;
};

// The masks of a node's blocks of slots, `MaskBits` each, block 0 lowest.
template <std::size_t MaskBits, std::size_t Blocks>
class block_masks {
 public:
  template <std::size_t Block>
  [[gnu::always_inline]] void set(std::uint32_t mask) noexcept {
    words_[Block / blocks_per_word] |= static_cast<std::uint64_t>(mask)
                                       << (Block % blocks_per_word * MaskBits);
  }

  // The set bits.
  [[gnu::always_inline]] std::size_t count() const noexcept { return count_from<0>(); }

  // The one-bits below the lowest zero bit.
  [[gnu::always_inline]] std::size_t low_ones() const noexcept { return low_run_from<0>(~std::uint64_t{0}); }

  // The zero bits below the lowest one-bit; every bit if none is set.
  [[gnu::always_inline]] std::size_t low_zeros() const noexcept { return low_run_from<0>(0); }

 private:
  static constexpr std::size_t blocks_per_word = 64 / MaskBits;
  static constexpr std::size_t words = (Blocks + blocks_per_word - 1) / blocks_per_word;

  template <std::size_t Word>
  [[gnu::always_inline]] std::size_t count_from() const noexcept {
    const auto here = static_cast<std::size_t>(std::popcount(words_[Word]));
    if constexpr (Word + 1 == words) {
      return here;
    } else {
      return here + count_from<Word + 1>();
    }
  }

  // The bits from word `Word` on that continue a run of bits equal to
  // `repeating`'s. A run goes on into the next word only through a word that
  // is all the run's bit, which is selected without a branch: where a run
  // ends is as unpredictable as the search's key.
  template <std::size_t Word>
  [[gnu::always_inline]] std::size_t low_run_from(std::uint64_t repeating) const noexcept {
    const std::uint64_t differing = words_[Word] ^ repeating;
    const auto here = static_cast<std::size_t>(std::countr_zero(differing));
    if constexpr (Word + 1 == words) {
      return here;
    } else {
      const std::size_t goes_on = std::size_t{0} - static_cast<std::size_t>(differing == 0);
      return here + (low_run_from<Word + 1>(repeating) & goes_on);
    }
  }

  std::array<std::uint64_t, words> words_{};
};

#if defined(__SSE2__)
// An SSE2 register of slots, 16 bytes of them.
template <class Slot>
struct sse2_lanes {
  using slot_type = Slot;
  using vector = __m128i;
  static constexpr std::size_t lanes = sizeof(vector) / sizeof(Slot);
  static constexpr std::size_t mask_bits = 16;
  static constexpr bool keeps_slot_order = true;
  // 64-bit lanes compare from SSE4.2 on.
#if defined(__SSE4_2__)
  static constexpr bool compares_slots = true;
#else
  static constexpr bool compares_slots = sizeof(Slot) < 8;
#endif

  static vector zero() noexcept { return _mm_setzero_si128(); }
  static vector load(const Slot* first) noexcept {
    return _mm_loadu_si128(reinterpret_cast<const vector*>(first));
  }
  static vector splat(Slot value) noexcept {
    if constexpr (sizeof(Slot) == 2) {
      return _mm_set1_epi16(static_cast<short>(value));
    } else if constexpr (sizeof(Slot) == 4) {
      return _mm_set1_epi32(static_cast<int>(value));
    } else {
      return _mm_set1_epi64x(static_cast<long long>(value));
    }
  }
  static vector exclusive_or(vector a, vector b) noexcept { return _mm_xor_si128(a, b); }

  // All ones in the lanes where `a` is greater than `b`, as signed numbers.
  static vector greater(vector a, vector b) noexcept {
    if constexpr (sizeof(Slot) == 2) {
      return _mm_cmpgt_epi16(a, b);
    } else if constexpr (sizeof(Slot) == 4) {
      return _mm_cmpgt_epi32(a, b);
    } else {
#if defined(__SSE4_2__)
      return _mm_cmpgt_epi64(a, b);
#else
      return zero();
#endif
    }
  }

  // Compare lanes of `LaneBytes`, each all ones or zero, at half their
  // width: `low`'s, then `high`'s.
  template <std::size_t LaneBytes>
  static vector narrowed(vector low, vector high) noexcept {
    if constexpr (LaneBytes == 4) {
      return _mm_packs_epi32(low, high);
    } else {
      return _mm_packs_epi16(low, high);
    }
  }

  // A bit per byte, the lowest byte's lowest.
  static std::uint32_t byte_mask(vector bytes) noexcept {
    return static_cast<std::uint32_t>(_mm_movemask_epi8(bytes));
  }

  // A last vector's compare lanes without its `Repeated` lowest, which the
  // vector before holds too: the others moved down, zeros after them, so
  // that every lane follows the one of the slot before.
  template <std::size_t Repeated>
  static vector fresh_lanes(vector compared) noexcept {
    return _mm_srli_si128(compared, static_cast<int>(Repeated * sizeof(Slot)));
  }
};
#endif

#if defined(__AVX2__)
// An AVX2 register of slots, 32 bytes of them.
template <class Slot>
struct avx2_lanes {
  using slot_type = Slot;
  using vector = __m256i;
  static constexpr std::size_t lanes = sizeof(vector) / sizeof(Slot);
  static constexpr std::size_t mask_bits = 32;
  static constexpr bool keeps_slot_order = false;
  static constexpr bool compares_slots = true;

  static vector zero() noexcept { return _mm256_setzero_si256(); }
  static vector load(const Slot* first) noexcept {
    return _mm256_loadu_si256(reinterpret_cast<const vector*>(first));
  }
  static vector splat(Slot value) noexcept {
    if constexpr (sizeof(Slot) == 2) {
      return _mm256_set1_epi16(static_cast<short>(value));
    } else if constexpr (sizeof(Slot) == 4) {
      return _mm256_set1_epi32(static_cast<int>(value));
    } else {
      return _mm256_set1_epi64x(static_cast<long long>(value));
    }
  }
  static vector exclusive_or(vector a, vector b) noexcept { return _mm256_xor_si256(a, b); }

  // All ones in the lanes where `a` is greater than `b`, as signed numbers.
  static vector greater(vector a, vector b) noexcept {
    if constexpr (sizeof(Slot) == 2) {
      return _mm256_cmpgt_epi16(a, b);
    } else if constexpr (sizeof(Slot) == 4) {
      return _mm256_cmpgt_epi32(a, b);
    } else {
      return _mm256_cmpgt_epi64(a, b);
    }
  }

  // Compare lanes of `LaneBytes`, each all ones or zero, at half their
  // width: in each half of the register `low`'s, then `high`'s, so not in
  // the slots' order.
  template <std::size_t LaneBytes>
  static vector narrowed(vector low, vector high) noexcept {
    if constexpr (LaneBytes == 4) {
      return _mm256_packs_epi32(low, high);
    } else {
      return _mm256_packs_epi16(low, high);
    }
  }

  // A bit per byte, the lowest byte's lowest.
  static std::uint32_t byte_mask(vector bytes) noexcept {
    return static_cast<std::uint32_t>(_mm256_movemask_epi8(bytes));
  }

  // A last vector's compare lanes without its `Repeated` lowest, which the
  // vector before holds too: those cleared, the others where they were.
  template <std::size_t Repeated>
  static vector fresh_lanes(vector compared) noexcept {
    static constexpr std::array<Slot, lanes> fresh = [] {
      std::array<Slot, lanes> ones{};
      for (std::size_t lane = Repeated; lane < lanes; ++lane) {
        ones[lane] = static_cast<Slot>(~Slot{0});
      }
      return ones;
    }();
    return _mm256_and_si256(compared, load(fresh.data()));
  }
};
#endif

// The count of `Capacity` slots in registers of `Lanes`: `sse2_lanes` or
// `avx2_lanes` of the slots' type.
template <class Lanes, std::size_t Capacity>
class vector_count {
  using slot_type = typename Lanes::slot_type;
  using vector = typename Lanes::vector;
  static constexpr std::size_t lanes = Lanes::lanes;

 public:
  // Whether the register compares slots of this width, and the slots fill
  // one, so that a load of their last `lanes` stays inside them.
  static constexpr bool fits = Lanes::compares_slots && Capacity >= lanes;

  // Exact for sorted slots where those past the node's length hold the
  // greatest inline key (invariant 4); for any slots, `below <= through`.
  [[gnu::always_inline]] static slot_counts of(const slot_type* slots, slot_type target) noexcept {
    return vector_count(slots, target).counts(std::make_index_sequence<blocks>());
  }

 private:
  // The registers compare as signed numbers, so the slots and the target
  // flip their sign bit.
  static constexpr slot_type sign_bit = static_cast<slot_type>(slot_type{1} << (8 * sizeof(slot_type) - 1));

  // Whole vectors from slot 0, and for a remainder a last one of the last
  // `lanes` slots, of which `repeated` are the vector before's.
  static constexpr std::size_t vectors = (Capacity + lanes - 1) / lanes;
  static constexpr std::size_t repeated = vectors * lanes - Capacity;

  // A compare's lanes are the slots', or the two 4-byte halves of an 8-byte
  // slot's; packing twice from 4 bytes, once from 2, leaves bytes. So a
  // block of 4 or 2 vectors packs to one register of bytes, and to one mask.
  static constexpr std::size_t compared_lane_bytes = std::min<std::size_t>(sizeof(slot_type), 4);
  static constexpr std::size_t packings = compared_lane_bytes / 2;
  static constexpr std::size_t block_vectors = std::size_t{1} << packings;
  static constexpr std::size_t blocks = (vectors + block_vectors - 1) / block_vectors;
  static constexpr std::size_t bits_per_slot = sizeof(slot_type) / compared_lane_bytes;

  using masks = block_masks<Lanes::mask_bits, blocks>;

  struct compare_lanes {
    vector below;
    vector above;
  };

  vector_count(const slot_type* slots, slot_type target) noexcept
      : slots_(slots), flipped_target_(Lanes::splat(static_cast<slot_type>(target ^ sign_bit))) {}

  // Vector `Vector`'s slots compared with the target; no lanes for a vector
  // past the last.
  template <std::size_t Vector>
  [[gnu::always_inline]] compare_lanes compared() const noexcept {
    if constexpr (Vector >= vectors) {
      return {Lanes::zero(), Lanes::zero()};
    } else {
      constexpr bool repeats = Vector + 1 == vectors && repeated != 0;
      const vector slots = Lanes::load(slots_ + (repeats ? Capacity - lanes : Vector * lanes));
      const vector flipped = Lanes::exclusive_or(slots, Lanes::splat(sign_bit));
      const compare_lanes whole{Lanes::greater(flipped_target_, flipped),
                                Lanes::greater(flipped, flipped_target_)};
      if constexpr (repeats) {
        return {Lanes::template fresh_lanes<repeated>(whole.below),
                Lanes::template fresh_lanes<repeated>(whole.above)};
      } else {
        return whole;
      }
    }
  }

  // The compares of the vectors from `First` on, as many as fill a register
  // when packed `Packings` times.
  template <std::size_t Packings, std::size_t First>
  [[gnu::always_inline]] compare_lanes packed() const noexcept {
    if constexpr (Packings == 0 || First >= vectors) {
      return compared<First>();
    } else {
      constexpr std::size_t lane_bytes = compared_lane_bytes >> (Packings - 1);
      const compare_lanes low = packed<Packings - 1, First>();
      const compare_lanes high = packed<Packings - 1, First + (std::size_t{1} << (Packings - 1))>();
      return {Lanes::template narrowed<lane_bytes>(low.below, high.below),
              Lanes::template narrowed<lane_bytes>(low.above, high.above)};
    }
  }

  template <std::size_t Block>
  [[gnu::always_inline]] void compare_block(masks& below, masks& above) const noexcept {
    const compare_lanes bytes = packed<packings, Block * block_vectors>();
    below.template set<Block>(Lanes::byte_mask(bytes.below));
    above.template set<Block>(Lanes::byte_mask(bytes.above));
  }

  // The blocks are named by a pack, not a loop, as are the vectors of a
  // block: straight-line code whether or not the compiler unrolls.
  template <std::size_t... Block>
  [[gnu::always_inline]] slot_counts counts(std::index_sequence<Block...>) const noexcept {
    masks below;
    masks above;
    (compare_block<Block>(below, above), ...);
    if constexpr (Lanes::keeps_slot_order) {
      return {below.low_ones() / bits_per_slot, above.low_zeros() / bits_per_slot};
    } else {
      return {below.count() / bits_per_slot, Capacity - above.count() / bits_per_slot};
    }
  }

  const slot_type* slots_;
  vector flipped_target_;
};

}  // namespace bbtree::detail
