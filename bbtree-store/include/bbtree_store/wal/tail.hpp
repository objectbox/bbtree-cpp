// The WAL's tail (`wal_tail`): where the WAL ends, as the writer knows it. A
// scan moves it over the last file's bytes (`wal_tail::scan_bytes`); an
// append goes after it, in the bytes `encoded_append` encodes.
//
// The Rust crate's Verus proofs show that a scan reads exactly what appends
// write: bytes past a tail scan to records exactly when they hold them, an
// append's bytes hold its records, the scan and the append give the same
// addresses for them and the same tail after them, and a scan picks up where
// it stopped. Their preconditions are assertions here; the tests check the
// rest (`wal_test.cpp`).
#pragma once

#include <bbtree_store/wal/record.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace bbtree_store::wal::detail {

// A WAL file's header: the magic, then the format's version, 1.
inline constexpr std::array<std::uint8_t, 12> file_header = {'b', 'b', 't', 'r', 'e', 'e',
                                                             'W', 'L', 1,   0,   0,   0};

// A record to append: its kind, key and value.
struct append_part {
  stored_kind kind;
  bytes key;
  bytes value;
};

struct scanned_bytes;

// Where the WAL ends, as the writer knows it.
struct wal_tail {
  // The last WAL file's number.
  std::uint64_t file = 0;
  // Its length: 0 while it doesn't exist.
  std::uint64_t file_len = 0;
  // The appends it holds.
  std::uint64_t file_appends = 0;
  // The LSN of the WAL's next record.
  std::uint64_t next_lsn = 0;

  // The tail at the start of the next file.
  wal_tail next_file() const noexcept {
    assert(file < std::numeric_limits<std::uint64_t>::max());
    return {file + 1, 0, 0, next_lsn};
  }

  // Where the next append goes: here, or at the start of the next file once
  // this one holds `max_appends`.
  wal_tail append_position(std::uint32_t max_appends) const noexcept {
    return file_appends == max_appends ? next_file() : *this;
  }

  // Scans `from`, the last WAL file's bytes past this tail: the records they
  // hold, with their addresses and the tail after them; else, with
  // `holds_records` false, where they stop holding records. Bytes hold
  // records when they are the header where the file starts, then records
  // whose LSNs follow from the tail's, the last ending an append.
  scanned_bytes scan_bytes(bytes from) const;
};

// What a scan read: records, their addresses, and the tail after them; or
// where the bytes stop holding records.
struct scanned_bytes {
  bool holds_records = false;
  std::size_t stopped_at = 0;
  std::vector<stored_record> records;
  std::vector<record_address> addresses;
  wal_tail tail;

  static scanned_bytes stopped(std::size_t at) {
    scanned_bytes stop;
    stop.stopped_at = at;
    return stop;
  }
};

inline scanned_bytes wal_tail::scan_bytes(bytes from) const {
  std::size_t pos = 0;
  if (file_len == 0) {
    for (std::size_t i = 0; i < file_header.size(); ++i) {
      if (i == from.size() || from[i] != file_header[i]) {
        return scanned_bytes::stopped(0);
      }
    }
    pos = file_header.size();
  }
  scanned_bytes scanned;
  // Where the last append scanned whole ends.
  std::size_t last_append_end = pos;
  std::uint64_t lsn = next_lsn;
  std::uint64_t appends = file_appends;
  while (pos < from.size()) {
    const std::optional<stored_record> decoded = stored_record::decode(from.subspan(pos));
    if (!decoded || decoded->lsn != lsn) {
      return scanned_bytes::stopped(pos);
    }
    const std::size_t len = decoded->len();
    scanned.addresses.emplace_back(file, file_len + pos, len);
    pos += len;
    ++lsn;
    if (decoded->last_in_append) {
      ++appends;
      last_append_end = pos;
    }
    scanned.records.push_back(*decoded);
  }
  if (last_append_end != pos) {
    return scanned_bytes::stopped(last_append_end);
  }
  scanned.holds_records = true;
  scanned.tail = {file, file_len + from.size(), appends, lsn};
  return scanned;
}

// One append's bytes, the addresses of its records, and the tail after it.
class encoded_append {
 public:
  // Encodes `parts`, at least one, each a record's kind, key and value of at
  // most 2^32 - 1 bytes, as an append at `position`: bytes that hold its
  // records there, their addresses, and the tail after them.
  encoded_append(const wal_tail& position, std::span<const append_part> parts)
      : file_(position.file), offset_(position.file_len) {
    assert(!parts.empty() && "an append holds a record");
    if (position.file_len == 0) {
      bytes_.insert(bytes_.end(), file_header.begin(), file_header.end());
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
      const std::size_t pos = bytes_.size();
      const stored_record encoded{position.next_lsn + i, parts[i].kind, i + 1 == parts.size(), parts[i].key,
                                  parts[i].value};
      encoded.encode(bytes_);
      addresses_.emplace_back(position.file, position.file_len + pos, bytes_.size() - pos);
    }
    tail_ = {position.file, position.file_len + bytes_.size(), position.file_appends + 1,
             position.next_lsn + parts.size()};
  }

  std::uint64_t file() const noexcept { return file_; }
  std::uint64_t offset() const noexcept { return offset_; }
  bytes encoded() const noexcept { return bytes_; }
  const std::vector<record_address>& addresses() const noexcept { return addresses_; }
  const wal_tail& tail() const noexcept { return tail_; }

 private:
  std::uint64_t file_;
  std::uint64_t offset_;
  std::vector<std::uint8_t> bytes_;
  std::vector<record_address> addresses_;
  wal_tail tail_;
};

}  // namespace bbtree_store::wal::detail
