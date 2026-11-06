// The fuzz targets' input: bytes read as operations, as the Rust targets'
// `arbitrary` decoding does. Reading past the end gives zeros, so every byte
// string is some sequence of operations.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace bbtree_fuzz {

class fuzz_input {
 public:
  explicit fuzz_input(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

  bool done() const noexcept { return at_ >= bytes_.size(); }

  std::uint8_t byte() noexcept { return at_ < bytes_.size() ? bytes_[at_++] : 0; }

  std::uint64_t u64() noexcept {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(byte()) << shift;
    }
    return value;
  }

  // A number below `bound`, from as few bytes as it needs.
  std::uint64_t below(std::uint64_t bound) noexcept {
    if (bound <= 1) {
      return 0;
    }
    if (bound <= 256) {
      return byte() % bound;
    }
    const std::uint64_t value = static_cast<std::uint64_t>(byte()) | static_cast<std::uint64_t>(byte()) << 8;
    return bound <= 65'536 ? value % bound : (value | u64() << 16) % bound;
  }

  // A short byte string over a small alphabet, so that keys share prefixes
  // and tie in their inline keys.
  std::string text(std::size_t max_len) {
    static constexpr char alphabet[] = {'\0', 'a', 'b', '/', '\xff', 'z'};
    std::string bytes(below(max_len + 1), '\0');
    for (char& c : bytes) {
      c = alphabet[below(sizeof alphabet)];
    }
    return bytes;
  }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t at_ = 0;
};

}  // namespace bbtree_fuzz
