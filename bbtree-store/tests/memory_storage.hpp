// WAL files in memory (`memory_storage`), for the tests: they keep S3
// Express's rules, and fail appends on demand (the Rust crate's
// `tests/wal/memory.rs`).
#pragma once

#include <bbtree_store/wal/storage.hpp>

#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace bbtree_store_test {

// How an append fails, set by `memory_storage::fail_append`. Either way, it
// reports `append_status::outcome_unknown`.
enum class append_fault : std::uint8_t {
  // The request is lost: the append doesn't happen.
  request_lost,
  // The response is lost: the append happens, written or not.
  response_lost,
};

// WAL files in memory, which take `MaxAppends` appends each. Its copies share
// its files, as handles to one storage.
template <std::uint32_t MaxAppends = 10'000>
class memory_storage {
 public:
  static constexpr std::uint32_t max_appends = MaxAppends;

  // Fails the next append that has no fault set yet with `fault`.
  void fail_append(append_fault fault) const {
    const std::lock_guard lock(files_->mutex);
    files_->faults.push_back(fault);
  }

  bbtree_store::wal::append_result append(std::uint64_t file, std::uint64_t offset,
                                          std::span<const std::uint8_t> bytes) const {
    const std::lock_guard lock(files_->mutex);
    const bbtree_store::wal::append_result lost{bbtree_store::wal::append_status::outcome_unknown,
                                                std::make_error_code(std::errc::timed_out)};
    std::optional<append_fault> fault;
    if (!files_->faults.empty()) {
      fault = files_->faults.front();
      files_->faults.pop_front();
    }
    if (fault == append_fault::request_lost) {
      return lost;
    }
    const bbtree_store::wal::append_result done = append_locked(file, offset, bytes);
    return fault == append_fault::response_lost ? lost : done;
  }

  std::optional<std::uint64_t> len(std::uint64_t file) const {
    const std::lock_guard lock(files_->mutex);
    const auto stored = files_->files.find(file);
    if (stored == files_->files.end()) {
      return std::nullopt;
    }
    return stored->second.bytes.size();
  }

  bool read(std::uint64_t file, std::uint64_t offset, std::span<std::uint8_t> buf) const {
    const std::lock_guard lock(files_->mutex);
    const auto stored = files_->files.find(file);
    if (stored == files_->files.end()) {
      throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    const std::vector<std::uint8_t>& bytes = stored->second.bytes;
    if (offset > bytes.size() || buf.size() > bytes.size() - offset) {
      return false;
    }
    if (!buf.empty()) {
      std::memcpy(buf.data(), bytes.data() + offset, buf.size());
    }
    return true;
  }

 private:
  struct memory_file {
    std::vector<std::uint8_t> bytes;
    std::uint32_t appends = 0;
  };
  struct shared_files {
    std::mutex mutex;
    std::map<std::uint64_t, memory_file> files;
    // The faults of the next appends, in order.
    std::deque<append_fault> faults;
  };

  bbtree_store::wal::append_result append_locked(std::uint64_t file, std::uint64_t offset,
                                                 std::span<const std::uint8_t> bytes) const {
    const auto stored = files_->files.find(file);
    const bool mismatch =
        stored == files_->files.end() ? offset != 0 : offset == 0 || stored->second.bytes.size() != offset;
    if (mismatch) {
      return {bbtree_store::wal::append_status::offset_mismatch, {}};
    }
    memory_file& target = files_->files[file];
    if (target.appends == MaxAppends) {
      return {bbtree_store::wal::append_status::outcome_unknown,
              std::make_error_code(std::errc::file_too_large)};
    }
    target.bytes.insert(target.bytes.end(), bytes.begin(), bytes.end());
    ++target.appends;
    return {bbtree_store::wal::append_status::appended, {}};
  }

  std::shared_ptr<shared_files> files_ = std::make_shared<shared_files>();
};

}  // namespace bbtree_store_test
