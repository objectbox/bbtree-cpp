// SHA-256 (FIPS 180-4), which `verify-data` checks the datasets with: the
// Rust bench's sha2 crate, without a dependency.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace bbtree_bench {

class sha256 {
 public:
  // The digest of `bytes`, in lowercase hexadecimal.
  static std::string hex_digest(std::span<const std::uint8_t> bytes);

  // The digest of the file at `path`, in lowercase hexadecimal.
  static std::string hex_digest_of_file(const std::filesystem::path& path);

 private:
  sha256() noexcept = default;

  void update(std::span<const std::uint8_t> bytes) noexcept;
  std::string finish_hex() noexcept;
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<std::uint8_t, 64> block_{};
  std::size_t block_length_ = 0;
  std::uint64_t total_bytes_ = 0;
};

}  // namespace bbtree_bench
