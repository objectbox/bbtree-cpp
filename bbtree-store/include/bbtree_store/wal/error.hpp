// `wal_error`: why a WAL operation failed (the Rust crate's `WalError`),
// thrown by the writer and by lookups (PORTING.md, P26).
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

namespace bbtree_store::wal {

enum class wal_error_kind : std::uint8_t {
  // The storage failed: an append's records may or may not have been
  // written. `code()` says how.
  storage,
  // The WAL's tail moved under the writer: another writer claimed the WAL,
  // or an append that failed was written after all. Open the WAL again.
  tail_moved,
  // The bytes at `offset()` of WAL file `file()` fail their checks.
  corrupted,
};

class wal_error : public std::runtime_error {
 public:
  static wal_error storage(std::error_code code) {
    return wal_error(wal_error_kind::storage, "the WAL's storage failed: " + code.message(), code, 0, 0);
  }
  static wal_error tail_moved() {
    return wal_error(wal_error_kind::tail_moved, "the WAL's tail moved under its writer", {}, 0, 0);
  }
  static wal_error corrupted(std::uint64_t file, std::uint64_t offset) {
    return wal_error(wal_error_kind::corrupted,
                     "WAL file " + std::to_string(file) + " is corrupted at offset " + std::to_string(offset),
                     {}, file, offset);
  }

  wal_error_kind kind() const noexcept { return kind_; }
  // The storage's error, for `wal_error_kind::storage`.
  std::error_code code() const noexcept { return code_; }
  // Where a corrupted WAL fails its checks, for `wal_error_kind::corrupted`.
  std::uint64_t file() const noexcept { return file_; }
  std::uint64_t offset() const noexcept { return offset_; }

 private:
  wal_error(wal_error_kind kind, const std::string& message, std::error_code code, std::uint64_t file,
            std::uint64_t offset)
      : std::runtime_error(message), kind_(kind), code_(code), file_(file), offset_(offset) {}

  wal_error_kind kind_;
  std::error_code code_;
  std::uint64_t file_;
  std::uint64_t offset_;
};

}  // namespace bbtree_store::wal
