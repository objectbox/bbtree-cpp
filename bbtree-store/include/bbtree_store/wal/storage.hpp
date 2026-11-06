// The storage under the WAL (`wal_storage`, design/bbtree-store/01-wal.md,
// "Storage"): numbered files that take appends at their end, under S3
// Express's rules on every backend. Synchronous: a call returns once the
// storage answered (PORTING.md, P25).
//
//   struct my_storage {
//     static constexpr std::uint32_t max_appends = 10'000;  // S3 Express's
//     // Appends `bytes` to file `file` if its length is `offset`; offset 0
//     // creates the file, which must not exist. Whole or nothing, and
//     // durable once it returns `appended`.
//     append_result append(std::uint64_t file, std::uint64_t offset,
//                          std::span<const std::uint8_t> bytes) const;
//     // File `file`'s length; none if it doesn't exist.
//     std::optional<std::uint64_t> len(std::uint64_t file) const;
//     // Reads `buf.size()` bytes of file `file` from `offset`; false if the
//     // file holds fewer.
//     bool read(std::uint64_t file, std::uint64_t offset, std::span<std::uint8_t> buf) const;
//   };
//
// Lookups read while the writer appends, from other threads: the calls are
// `const` and thread-safe. `len` and `read` throw `std::system_error` when the
// storage fails; `append` reports every failure but a mismatched offset as an
// unknown outcome, which the writer attempts again and reads back.
#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>

namespace bbtree_store::wal {

enum class append_status : std::uint8_t {
  // Written, whole, and durable.
  appended,
  // The offset isn't the file's length, or, for offset 0, the file exists.
  // Nothing was written.
  offset_mismatch,
  // The bytes may or may not have been written; `error` says why.
  outcome_unknown,
};

// What `wal_storage::append` did.
struct append_result {
  append_status status = append_status::appended;
  std::error_code error;
};

template <class S>
concept wal_storage = requires(const S& storage, std::uint64_t file, std::uint64_t offset,
                               std::span<const std::uint8_t> bytes, std::span<std::uint8_t> buf) {
                        { S::max_appends } -> std::convertible_to<std::uint32_t>;
                        { storage.append(file, offset, bytes) } -> std::same_as<append_result>;
                        { storage.len(file) } -> std::same_as<std::optional<std::uint64_t>>;
                        { storage.read(file, offset, buf) } -> std::same_as<bool>;
                      };

}  // namespace bbtree_store::wal
