#include "memory.hpp"

#include <mimalloc.h>

#include <new>
#include <string>

#include "text_file.hpp"
#include "text_lines.hpp"

namespace bbtree_bench {

void* counting_allocator::allocate(std::size_t size) noexcept {
  void* pointer = mi_malloc(size);
  count_allocated(pointer);
  return pointer;
}

void* counting_allocator::allocate_aligned(std::size_t size, std::size_t alignment) noexcept {
  void* pointer = mi_malloc_aligned(size, alignment);
  count_allocated(pointer);
  return pointer;
}

void* counting_allocator::allocate_or_throw(std::size_t size) {
  void* pointer = allocate(size);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

void* counting_allocator::allocate_aligned_or_throw(std::size_t size, std::size_t alignment) {
  void* pointer = allocate_aligned(size, alignment);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

void counting_allocator::deallocate(void* pointer) noexcept {
  if (pointer == nullptr) {
    return;
  }
  live_bytes_.fetch_sub(mi_usable_size(pointer), std::memory_order_relaxed);
  mi_free(pointer);
}

void counting_allocator::count_allocated(void* pointer) noexcept {
  if (pointer != nullptr) {
    live_bytes_.fetch_add(mi_usable_size(pointer), std::memory_order_relaxed);
  }
}

resident_set resident_set::read() {
  const std::string text = text_file::read("/proc/self/status").value_or("");
  return resident_set(field_bytes(text, "VmRSS:"), field_bytes(text, "VmHWM:"));
}

std::uint64_t resident_set::field_bytes(std::string_view status, std::string_view name) noexcept {
  std::uint64_t bytes = 0;
  bool found = false;
  text_lines::for_each(status, [&](std::string_view line) {
    if (found || !line.starts_with(name)) {
      return;
    }
    found = true;
    std::string_view value = text_lines::trimmed(line.substr(name.size()));
    if (!value.ends_with(" kB")) {
      return;
    }
    value.remove_suffix(3);
    if (const std::optional<std::uint64_t> kilobytes = text_lines::parse_u64(value)) {
      bytes = *kilobytes * 1024;
    }
  });
  return bytes;
}

}  // namespace bbtree_bench
