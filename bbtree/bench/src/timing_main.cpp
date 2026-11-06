// The timing binary: every cell is timed on plain mimalloc, which replaces
// `operator new` and `delete` here, as the Rust bench's
// `#[global_allocator]` does (PORTING.md, P17).
#include <mimalloc-new-delete.h>

#include "bench_program.hpp"

int main(int argc, char** argv) {
  return bbtree_bench::bench_program::run(argc, argv, bbtree_bench::run_mode::timing());
}
