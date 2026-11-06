#include "sha256.hpp"

#include <bit>
#include <format>
#include <fstream>
#include <vector>

#include "named.hpp"

namespace bbtree_bench {

namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

}  // namespace

std::string sha256::hex_digest(std::span<const std::uint8_t> bytes) {
  sha256 hasher;
  hasher.update(bytes);
  return hasher.finish_hex();
}

std::string sha256::hex_digest_of_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw bench_error(std::format("{}: cannot open", path.string()));
  }
  sha256 hasher;
  std::vector<char> buffer(std::size_t{1} << 20);
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read = static_cast<std::size_t>(file.gcount());
    hasher.update(std::span(reinterpret_cast<const std::uint8_t*>(buffer.data()), read));
  }
  if (!file.eof()) {
    throw bench_error(std::format("reading {} failed", path.string()));
  }
  return hasher.finish_hex();
}

void sha256::update(std::span<const std::uint8_t> bytes) noexcept {
  total_bytes_ += bytes.size();
  for (const std::uint8_t byte : bytes) {
    block_[block_length_++] = byte;
    if (block_length_ == block_.size()) {
      compress(block_.data());
      block_length_ = 0;
    }
  }
}

std::string sha256::finish_hex() noexcept {
  const std::uint64_t total_bits = total_bytes_ * 8;
  block_[block_length_++] = 0x80;
  if (block_length_ > 56) {
    while (block_length_ < 64) {
      block_[block_length_++] = 0;
    }
    compress(block_.data());
    block_length_ = 0;
  }
  while (block_length_ < 56) {
    block_[block_length_++] = 0;
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    block_[block_length_++] = static_cast<std::uint8_t>(total_bits >> shift);
  }
  compress(block_.data());
  std::string hex;
  for (const std::uint32_t word : state_) {
    hex += std::format("{:08x}", word);
  }
  return hex;
}

void sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = static_cast<std::uint32_t>(block[4 * index]) << 24 |
                      static_cast<std::uint32_t>(block[4 * index + 1]) << 16 |
                      static_cast<std::uint32_t>(block[4 * index + 2]) << 8 |
                      static_cast<std::uint32_t>(block[4 * index + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t w15 = schedule[index - 15];
    const std::uint32_t w2 = schedule[index - 2];
    const std::uint32_t s0 = std::rotr(w15, 7) ^ std::rotr(w15, 18) ^ (w15 >> 3);
    const std::uint32_t s1 = std::rotr(w2, 17) ^ std::rotr(w2, 19) ^ (w2 >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }
  auto [a, b, c, d, e, f, g, h] = state_;
  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + s1 + choice + round_constants[index] + schedule[index];
    const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  const std::array<std::uint32_t, 8> worked{a, b, c, d, e, f, g, h};
  for (std::size_t index = 0; index < 8; ++index) {
    state_[index] += worked[index];
  }
}

}  // namespace bbtree_bench
