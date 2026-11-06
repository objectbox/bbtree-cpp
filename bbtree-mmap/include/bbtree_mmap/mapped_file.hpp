// A map file's bytes: mapped from a file, read into memory, or written.
#pragma once

#include <bbtree/file.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bbtree_mmap {

// What OS pages a mapping asks for: the base pages, or transparent huge pages
// (2 MiB on x86-64), so that lookups over a large map miss the TLB less.
enum class os_pages : std::uint8_t { base, huge };

class mapped_file {
 public:
  // Maps the bytes `range_start..range_start + range_len` of the file at
  // `path` (all of it for `range_len` 0) read-only. Nothing may change the
  // file while it is mapped.
  static mapped_file map(const std::filesystem::path& path, os_pages pages, std::uint64_t range_start = 0,
                         std::uint64_t range_len = 0) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      throw std::system_error(errno, std::generic_category(), "bbtree_mmap: open");
    }
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
      const int error = errno;
      ::close(fd);
      throw std::system_error(error, std::generic_category(), "bbtree_mmap: fstat");
    }
    const auto file_len = static_cast<std::uint64_t>(status.st_size);
    if (range_len == 0) {
      range_len = file_len - range_start;
    }
    if (range_start + range_len > file_len) {
      ::close(fd);
      throw std::out_of_range("bbtree_mmap: range past the file's end");
    }
    const auto os_page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    const std::uint64_t mapped_start = range_start / os_page * os_page;
    const std::uint64_t lead = range_start - mapped_start;
    void* mapped =
        ::mmap(nullptr, lead + range_len, PROT_READ, MAP_PRIVATE, fd, static_cast<off_t>(mapped_start));
    ::close(fd);
    if (mapped == MAP_FAILED) {
      throw std::system_error(errno, std::generic_category(), "bbtree_mmap: mmap");
    }
    if (pages == os_pages::huge) {
      (void)::madvise(mapped, lead + range_len, MADV_HUGEPAGE);
    }
    mapped_file file;
    file.mapping_ = static_cast<std::byte*>(mapped);
    file.mapping_len_ = lead + range_len;
    file.bytes_ = {file.mapping_ + lead, range_len};
    return file;
  }

  // Reads the whole file at `path` into memory, aligned to `alignment`.
  static mapped_file read(const std::filesystem::path& path, std::size_t alignment) {
    const auto size = static_cast<std::size_t>(std::filesystem::file_size(path));
    mapped_file file;
    file.buf_ = bbtree::file_buf(size, alignment);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      throw std::system_error(errno, std::generic_category(), "bbtree_mmap: open");
    }
    std::size_t done = 0;
    while (done < size) {
      const ssize_t got = ::read(fd, file.buf_.bytes().data() + done, size - done);
      if (got <= 0) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "bbtree_mmap: read");
      }
      done += static_cast<std::size_t>(got);
    }
    ::close(fd);
    file.bytes_ = file.buf_.bytes();
    return file;
  }

  // Writes `bytes` to a new file at `path`, replacing any.
  static void write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
      throw std::system_error(errno, std::generic_category(), "bbtree_mmap: create");
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t wrote = ::write(fd, bytes.data() + done, bytes.size() - done);
      if (wrote <= 0) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "bbtree_mmap: write");
      }
      done += static_cast<std::size_t>(wrote);
    }
    ::close(fd);
  }

  mapped_file(mapped_file&& other) noexcept
      : mapping_(std::exchange(other.mapping_, nullptr)),
        mapping_len_(std::exchange(other.mapping_len_, 0)),
        buf_(std::move(other.buf_)),
        bytes_(std::exchange(other.bytes_, {})) {}
  mapped_file(const mapped_file&) = delete;
  mapped_file& operator=(const mapped_file&) = delete;
  ~mapped_file() {
    if (mapping_ != nullptr) {
      ::munmap(mapping_, mapping_len_);
    }
  }

  std::span<const std::byte> bytes() const noexcept { return bytes_; }

 private:
  mapped_file() noexcept = default;

  std::byte* mapping_ = nullptr;
  std::size_t mapping_len_ = 0;
  bbtree::file_buf buf_;
  std::span<const std::byte> bytes_;
};

}  // namespace bbtree_mmap
