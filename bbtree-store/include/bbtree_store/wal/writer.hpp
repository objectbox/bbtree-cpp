// The WAL (design/bbtree-store/01-wal.md): records, each with its LSN and a
// CRC, appended to numbered WAL files in a `wal_storage`; and its one writer
// (`wal_writer`), which opens the WAL, scanning it and claiming it, then
// appends. It leaves what the bytes are to the tail (`tail.hpp`).
//
// - A scan reads each WAL file from the tail on, has the tail scan the bytes,
//   and replays the inserts and removals they hold; then it moves to the next
//   file, if there is one.
// - An append goes where the tail says, in the bytes it encodes. An attempt
//   whose outcome is unknown is made again, same bytes at the same offset; if
//   the offset then mismatches, the writer reads the bytes back to learn
//   whether an earlier attempt wrote them.
// - Opening scans, then appends a claim, scanning on and claiming again for
//   as long as another writer appended first. The claim fences the writer
//   before it.
// - Lookups read a record back by its address (`record_address::read_value`)
//   while the writer appends.
//
//   auto writer = wal_writer<my_storage>::open(storage, [](record_address, const record&) {});
//   writer.append(std::array{record::insert(key, value)});   // their addresses
#pragma once

#include <bbtree_store/wal/error.hpp>
#include <bbtree_store/wal/record.hpp>
#include <bbtree_store/wal/storage.hpp>
#include <bbtree_store/wal/tail.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace bbtree_store::wal {

// The WAL's one writer. Throws `wal_error`.
template <wal_storage S>
class wal_writer {
  using tail_type = detail::wal_tail;

 public:
  // Scans the WAL in `storage`, giving `replay(address, record)` each record
  // but claims, in LSN order; then claims the WAL.
  template <class Replay>
  static wal_writer open(S storage, Replay&& replay) {
    // Random: each open is a new writer, with an id of its own.
    std::random_device entropy;
    const std::uint64_t id = (std::uint64_t{entropy()} << 32) | entropy();
    std::vector<std::uint8_t> writer_id;
    detail::little_endian::push(writer_id, id);
    const detail::append_part claim[] = {{detail::stored_kind::claim, writer_id, {}}};
    tail_type tail;
    while (true) {
      scan(storage, tail, replay);
      const detail::encoded_append append(tail.append_position(S::max_appends), claim);
      if (write(storage, append)) {
        return wal_writer(std::move(storage), append.tail());
      }
      // Another writer appended since the scan: scan on.
    }
  }

  // Appends `records` as one append; gives their addresses. Appends nothing
  // for no records. Throws `std::length_error` for a key or a value longer
  // than 2^32 - 1 bytes.
  std::vector<record_address> append(std::span<const record> records) {
    if (records.empty()) {
      return {};
    }
    std::vector<detail::append_part> parts;
    parts.reserve(records.size());
    constexpr std::size_t max_len = std::numeric_limits<std::uint32_t>::max();
    for (const record& each : records) {
      if (each.key.size() > max_len || each.value.size() > max_len) {
        throw std::length_error("bbtree_store: a WAL record's key or value past 2^32 - 1 bytes");
      }
      const auto kind =
          each.kind == record::kind_type::insert ? detail::stored_kind::insert : detail::stored_kind::remove;
      parts.push_back({kind, each.key, each.value});
    }
    const detail::encoded_append encoded(tail_.append_position(S::max_appends), parts);
    if (!write(storage_, encoded)) {
      throw wal_error::tail_moved();
    }
    tail_ = encoded.tail();
    return encoded.addresses();
  }

  const S& storage() const noexcept { return storage_; }

 private:
  // Attempts at an append, while its outcome is unknown, before it fails.
  static constexpr int append_attempts = 3;

  wal_writer(S storage, const tail_type& tail) : storage_(std::move(storage)), tail_(tail) {}

  // Reads the WAL from `tail` to its end, giving `replay` each record but
  // claims, and moves `tail` there.
  template <class Replay>
  static void scan(const S& storage, tail_type& tail, Replay& replay) {
    std::optional<std::uint64_t> file_len = len_of(storage, tail.file);
    while (file_len) {
      scan_file(storage, tail, *file_len, replay);
      file_len = len_of(storage, tail.file + 1);
      if (file_len) {
        tail = tail.next_file();
      }
    }
  }

  // Reads the last file from `tail` to its end, `file_len`, and moves `tail`
  // there. Reads it whole.
  template <class Replay>
  static void scan_file(const S& storage, tail_type& tail, std::uint64_t file_len, Replay& replay) {
    if (file_len < tail.file_len) {
      throw wal_error::corrupted(tail.file, tail.file_len);
    }
    std::vector<std::uint8_t> unread(static_cast<std::size_t>(file_len - tail.file_len));
    if (!read_from(storage, tail.file, tail.file_len, unread)) {
      throw wal_error::corrupted(tail.file, tail.file_len);
    }
    const detail::scanned_bytes scanned = tail.scan_bytes(unread);
    if (!scanned.holds_records) {
      throw wal_error::corrupted(tail.file, tail.file_len + scanned.stopped_at);
    }
    for (std::size_t i = 0; i < scanned.records.size(); ++i) {
      const detail::stored_record& stored = scanned.records[i];
      switch (stored.kind) {
        case detail::stored_kind::insert:
          replay(scanned.addresses[i], record::insert(stored.key, stored.value));
          break;
        case detail::stored_kind::remove:
          replay(scanned.addresses[i], record::remove(stored.key));
          break;
        case detail::stored_kind::claim:
          break;
      }
    }
    tail = scanned.tail;
  }

  // Writes the append's bytes to the storage, attempting again while the
  // outcome is unknown; false if the offset isn't the file's length, unless
  // an earlier attempt wrote these bytes there.
  static bool write(const S& storage, const detail::encoded_append& append) {
    for (int attempt = 1;; ++attempt) {
      const append_result result = storage.append(append.file(), append.offset(), append.encoded());
      switch (result.status) {
        case append_status::appended:
          return true;
        case append_status::offset_mismatch:
          return attempt > 1 && is_stored(storage, append);
        case append_status::outcome_unknown:
          if (attempt == append_attempts) {
            throw wal_error::storage(result.error);
          }
          break;
      }
    }
  }

  // Whether the storage holds the append's bytes at its offset.
  static bool is_stored(const S& storage, const detail::encoded_append& append) {
    std::vector<std::uint8_t> stored(append.encoded().size());
    if (!read_from(storage, append.file(), append.offset(), stored)) {
      return false;
    }
    return std::equal(stored.begin(), stored.end(), append.encoded().begin());
  }

  // The storage's calls, its failures as `wal_error`s.
  static std::optional<std::uint64_t> len_of(const S& storage, std::uint64_t file) {
    try {
      return storage.len(file);
    } catch (const std::system_error& error) {
      throw wal_error::storage(error.code());
    }
  }
  static bool read_from(const S& storage, std::uint64_t file, std::uint64_t offset,
                        std::span<std::uint8_t> buf) {
    try {
      return storage.read(file, offset, buf);
    } catch (const std::system_error& error) {
      throw wal_error::storage(error.code());
    }
  }

  S storage_;
  tail_type tail_;
};

}  // namespace bbtree_store::wal
