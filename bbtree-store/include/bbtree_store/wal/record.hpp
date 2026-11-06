// Records: the caller's (`record`), and as the WAL stores them
// (`stored_record`), which the writer encodes, and the scan and lookups
// (`record_address::read_value`) decode and check:
//
//   | crc: u32 | LSN: u64 | key length: u32 | value length: u32 | kind: u8 | key ... | value ... |
//
// little-endian and unaligned, the CRC32C of every byte after it. The kind is
// an insert, a removal or a claim, plus 0x80 on an append's last record.
//
// The Rust crate's Verus proofs show that the encoding round-trips: a record
// decodes from its bytes, whatever follows them, and nothing else does
// (PORTING.md, P1). Here the tests check it (`wal_test.cpp`).
#pragma once

#include <bbtree_store/wal/crc32c.hpp>
#include <bbtree_store/wal/error.hpp>
#include <bbtree_store/wal/storage.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace bbtree_store::wal {

using bytes = std::span<const std::uint8_t>;

// One key's insert or removal.
struct record {
  enum class kind_type : std::uint8_t { insert, remove };

  kind_type kind = kind_type::insert;
  bytes key;
  // An insert's value; empty for a removal.
  bytes value;

  static record insert(bytes key, bytes value) noexcept { return {kind_type::insert, key, value}; }
  static record remove(bytes key) noexcept { return {kind_type::remove, key, {}}; }
};

namespace detail {

// A record's kind as the WAL stores it.
enum class stored_kind : std::uint8_t {
  insert = 1,
  remove = 2,
  // A writer's claim on the WAL, its key the writer's id.
  claim = 3,
};

// Little-endian words, read from and written into byte strings.
struct little_endian {
  template <class Word>
  static void push(std::vector<std::uint8_t>& out, Word word) {
    for (std::size_t byte = 0; byte < sizeof(Word); ++byte) {
      out.push_back(static_cast<std::uint8_t>(word >> (8 * byte)));
    }
  }
  template <class Word>
  static Word read(bytes from, std::size_t at) noexcept {
    Word word = 0;
    for (std::size_t byte = 0; byte < sizeof(Word); ++byte) {
      word = static_cast<Word>(word | static_cast<Word>(from[at + byte]) << (8 * byte));
    }
    return word;
  }
  static void write_u32(std::span<std::uint8_t> out, std::size_t at, std::uint32_t word) noexcept {
    for (std::size_t byte = 0; byte < sizeof word; ++byte) {
      out[at + byte] = static_cast<std::uint8_t>(word >> (8 * byte));
    }
  }
};

// A record as the WAL stores it.
struct stored_record {
  // A record's bytes before its key: CRC, LSN, both lengths and kind.
  static constexpr std::size_t header_len = 21;

  std::uint64_t lsn = 0;
  stored_kind kind = stored_kind::insert;
  bool last_in_append = false;
  bytes key;
  bytes value;

  // Writes the record's bytes to the end of `out`; its key and value are at
  // most 2^32 - 1 bytes.
  void encode(std::vector<std::uint8_t>& out) const {
    assert(fits() && "a record's lengths are u32s");
    const std::size_t start = out.size();
    // A zero CRC, written over once the CRC of the bytes after it is known.
    little_endian::push(out, std::uint32_t{0});
    little_endian::push(out, lsn);
    little_endian::push(out, static_cast<std::uint32_t>(key.size()));
    little_endian::push(out, static_cast<std::uint32_t>(value.size()));
    out.push_back(kind_byte());
    out.insert(out.end(), key.begin(), key.end());
    out.insert(out.end(), value.begin(), value.end());
    const std::uint32_t crc = crc32c::of(bytes(out).subspan(start + 4));
    little_endian::write_u32(out, start, crc);
  }

  // The record at the start of `from`, if one is: it decodes a record only if
  // `from` starts with its bytes, and every record it starts with.
  static std::optional<stored_record> decode(bytes from) noexcept {
    if (from.size() < header_len) {
      return std::nullopt;
    }
    const auto crc = little_endian::read<std::uint32_t>(from, 0);
    const auto lsn = little_endian::read<std::uint64_t>(from, 4);
    const std::size_t key_len = little_endian::read<std::uint32_t>(from, 12);
    const std::size_t value_len = little_endian::read<std::uint32_t>(from, 16);
    const std::optional<stored_record> kind = of_kind_byte(from[20]);
    if (!kind || key_len > from.size() - header_len) {
      return std::nullopt;
    }
    const std::size_t key_end = header_len + key_len;
    if (value_len > from.size() - key_end) {
      return std::nullopt;
    }
    const std::size_t len = key_end + value_len;
    if (crc32c::of(from.subspan(4, len - 4)) != crc) {
      return std::nullopt;
    }
    stored_record decoded = *kind;
    decoded.lsn = lsn;
    decoded.key = from.subspan(header_len, key_len);
    decoded.value = from.subspan(key_end, value_len);
    return decoded;
  }

  // Its length in bytes.
  std::size_t len() const noexcept { return header_len + key.size() + value.size(); }

  // Whether its key's and value's lengths fit the u32s its bytes keep them in.
  bool fits() const noexcept {
    constexpr std::size_t max = std::numeric_limits<std::uint32_t>::max();
    return key.size() <= max && value.size() <= max;
  }

 private:
  // The kind byte: 1, 2 or 3, plus 0x80 on an append's last record.
  std::uint8_t kind_byte() const noexcept {
    const auto byte = static_cast<std::uint8_t>(kind);
    return last_in_append ? static_cast<std::uint8_t>(byte + 0x80) : byte;
  }

  // A record of the kind a kind byte says, and whether it is its append's
  // last; none for a byte no kind has.
  static std::optional<stored_record> of_kind_byte(std::uint8_t byte) noexcept {
    const bool last = (byte & 0x80) != 0;
    const auto kind = static_cast<std::uint8_t>(byte & 0x7f);
    if (kind < 1 || kind > 3) {
      return std::nullopt;
    }
    stored_record of_kind;
    of_kind.kind = static_cast<stored_kind>(kind);
    of_kind.last_in_append = last;
    return of_kind;
  }
};

}  // namespace detail

// Where a record lies: its WAL file, its offset in it, and its length. The
// index keeps it, to read the record back.
class record_address {
 public:
  record_address(std::uint64_t file, std::uint64_t offset, std::uint64_t len) noexcept
      : file_(file), offset_(offset), len_(len) {}

  std::uint64_t file() const noexcept { return file_; }
  std::uint64_t offset() const noexcept { return offset_; }
  std::uint64_t len() const noexcept { return len_; }
  friend bool operator==(const record_address&, const record_address&) = default;

  // Reads the insert here from `storage`, and gives its value. Throws
  // `wal_error`: corrupted if the bytes there aren't an insert, or storage.
  template <wal_storage S>
  std::vector<std::uint8_t> read_value(const S& storage) const {
    std::vector<std::uint8_t> stored(static_cast<std::size_t>(len_));
    try {
      if (!storage.read(file_, offset_, stored)) {
        throw wal_error::corrupted(file_, offset_);
      }
    } catch (const std::system_error& error) {
      throw wal_error::storage(error.code());
    }
    const std::optional<detail::stored_record> decoded = detail::stored_record::decode(stored);
    if (!decoded || decoded->len() != stored.size() || decoded->kind != detail::stored_kind::insert) {
      throw wal_error::corrupted(file_, offset_);
    }
    const std::size_t value_len = decoded->value.size();
    stored.erase(stored.begin(), stored.end() - static_cast<std::ptrdiff_t>(value_len));
    return stored;
  }

 private:
  std::uint64_t file_;
  std::uint64_t offset_;
  std::uint64_t len_;
};

}  // namespace bbtree_store::wal
