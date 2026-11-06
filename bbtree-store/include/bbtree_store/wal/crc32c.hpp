// CRC32C (Castagnoli), the WAL's record checksum, as the Rust crate's
// `crc32c` crate computes it, and S3 Express checks appends with: the
// reflected polynomial 0x82F63B78, all ones in, all ones out. Table-driven,
// a byte at a time (PORTING.md, P27).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bbtree_store::wal::detail {

struct crc32c {
  // The CRC32C of `bytes`.
  static std::uint32_t of(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = ~std::uint32_t{0};
    for (const std::uint8_t byte : bytes) {
      crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
  }

 private:
  static constexpr std::uint32_t polynomial = 0x82F63B78;

  // The CRC of each byte value, for a byte at a time.
  static constexpr std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> crcs{};
    for (std::uint32_t value = 0; value < 256; ++value) {
      std::uint32_t crc = value;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1) != 0 ? (crc >> 1) ^ polynomial : crc >> 1;
      }
      crcs[value] = crc;
    }
    return crcs;
  }();
};

}  // namespace bbtree_store::wal::detail
