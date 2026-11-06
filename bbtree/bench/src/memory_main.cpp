// The memory binary: mimalloc wrapped in a counter of live bytes, which every
// cell reads to report what its maps hold.
//
// The replaced `operator new` and `delete` must reach the one counting
// allocator, so it is the binary's one mutable global: the Rust bench's
// `static ALLOCATOR: CountingAllocator`. `constinit` makes it exist before
// any allocation. Every form is replaced, so none falls back to the default
// allocator and frees a mimalloc block with glibc.
#include <cstddef>
#include <new>

#include "bench_program.hpp"

namespace {

constinit bbtree_bench::counting_allocator live_bytes_counter;

}  // namespace

void* operator new(std::size_t size) { return live_bytes_counter.allocate_or_throw(size); }
void* operator new[](std::size_t size) { return live_bytes_counter.allocate_or_throw(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return live_bytes_counter.allocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return live_bytes_counter.allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  return live_bytes_counter.allocate_aligned_or_throw(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return live_bytes_counter.allocate_aligned_or_throw(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return live_bytes_counter.allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return live_bytes_counter.allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer) noexcept { live_bytes_counter.deallocate(pointer); }
void operator delete[](void* pointer) noexcept { live_bytes_counter.deallocate(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  live_bytes_counter.deallocate(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  live_bytes_counter.deallocate(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept { live_bytes_counter.deallocate(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { live_bytes_counter.deallocate(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { live_bytes_counter.deallocate(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { live_bytes_counter.deallocate(pointer); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  live_bytes_counter.deallocate(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  live_bytes_counter.deallocate(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  live_bytes_counter.deallocate(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  live_bytes_counter.deallocate(pointer);
}

int main(int argc, char** argv) {
  return bbtree_bench::bench_program::run(argc, argv, bbtree_bench::run_mode::memory(live_bytes_counter));
}
