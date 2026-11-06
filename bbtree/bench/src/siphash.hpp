// SipHash-1-3 with zero keys: the hash of Rust's `DefaultHasher` and of the
// siphasher crate's `SipHasher13::new()`, which the Rust bench pins its
// string keys' distinctness and its datasets' hold-out to.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace bbtree_bench {

class sip_hash_13 {
 public:
  // The hash of `bytes`, as `SipHasher13::new().hash(bytes)`.
  static std::uint64_t of_bytes(std::span<const std::uint8_t> bytes) noexcept {
    sip_hash_13 hasher;
    hasher.write(bytes);
    return hasher.finish();
  }

  static std::uint64_t of_bytes(std::string_view text) noexcept { return of_bytes(as_bytes(text)); }

  // The hash that a `str` feeds `DefaultHasher` through `Hash`: its bytes,
  // then 0xFF, which no UTF-8 string holds, to be prefix-free.
  static std::uint64_t of_rust_str(std::string_view text) noexcept {
    sip_hash_13 hasher;
    hasher.write(as_bytes(text));
    hasher.push_byte(0xFF);
    return hasher.finish();
  }

 private:
  static std::span<const std::uint8_t> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
  }

  // The bytes are a stream of little-endian words: whole words at once
  // where the stream is at a word boundary, byte by byte elsewhere.
  void write(std::span<const std::uint8_t> bytes) noexcept {
    std::size_t at = 0;
    while (at < bytes.size() && length_ % 8 != 0) {
      push_byte(bytes[at++]);
    }
    while (bytes.size() - at >= 8) {
      std::uint64_t word;
      std::memcpy(&word, bytes.data() + at, sizeof word);
      if constexpr (std::endian::native == std::endian::big) {
        word = __builtin_bswap64(word);
      }
      compress_word(word);
      length_ += 8;
      at += 8;
    }
    while (at < bytes.size()) {
      push_byte(bytes[at++]);
    }
  }

  void push_byte(std::uint8_t byte) noexcept {
    tail_ |= static_cast<std::uint64_t>(byte) << (8 * (length_ % 8));
    ++length_;
    if (length_ % 8 == 0) {
      compress_word(tail_);
      tail_ = 0;
    }
  }

  std::uint64_t finish() noexcept {
    compress_word(tail_ | (static_cast<std::uint64_t>(length_ & 0xFF) << 56));
    v2_ ^= 0xFF;
    for (int round = 0; round < 3; ++round) {
      sip_round();
    }
    return v0_ ^ v1_ ^ v2_ ^ v3_;
  }

  // One compression round per word: the "1" of SipHash-1-3.
  void compress_word(std::uint64_t word) noexcept {
    v3_ ^= word;
    sip_round();
    v0_ ^= word;
  }

  void sip_round() noexcept {
    v0_ += v1_;
    v1_ = std::rotl(v1_, 13);
    v1_ ^= v0_;
    v0_ = std::rotl(v0_, 32);
    v2_ += v3_;
    v3_ = std::rotl(v3_, 16);
    v3_ ^= v2_;
    v0_ += v3_;
    v3_ = std::rotl(v3_, 21);
    v3_ ^= v0_;
    v2_ += v1_;
    v1_ = std::rotl(v1_, 17);
    v1_ ^= v2_;
    v2_ = std::rotl(v2_, 32);
  }

  // The initial state for keys k0 = k1 = 0.
  std::uint64_t v0_ = 0x736f'6d65'7073'6575;
  std::uint64_t v1_ = 0x646f'7261'6e64'6f6d;
  std::uint64_t v2_ = 0x6c79'6765'6e65'7261;
  std::uint64_t v3_ = 0x7465'6462'7974'6573;
  std::uint64_t tail_ = 0;
  std::size_t length_ = 0;
};

}  // namespace bbtree_bench
