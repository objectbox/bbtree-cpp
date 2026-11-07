// The timing binary on the system's allocator: every cell is timed with the
// global `operator new` and `delete` left as the C++ runtime has them, glibc's
// `malloc` on Linux, which is what a map gets by default. The other two
// binaries run on mimalloc, as the Rust bench does (PORTING.md, P17).
#include "bench_program.hpp"

int main(int argc, char** argv) {
  return bbtree_bench::bench_program::run(argc, argv, bbtree_bench::run_mode::timing_on_system_allocator());
}
