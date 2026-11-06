// bbtree++'s benchmarks, a port of the Rust crate's bench (`bbtree/bench`,
// the Rust bench below): `bbtree::unsync::bbtree_map` (`bbtree-unsync`), and
// `bbtree::bbtree_map` (`bbtree`, the map threads share, where `--map` names
// it), against `std::map`, `absl::btree_map`, ankerl's `unordered_dense::map`
// and `std::unordered_map`, in the Rust bench's suites, cells, keys and CSV
// columns, so that a C++ run and a Rust run compare cell by cell
// (`compare`).
//
// - A *workload* is a key kind, a value kind, a size and an operation; a
//   *cell* is a workload on one map. Suites are lists of workloads
//   (`suites.hpp`), and each workload runs on every map.
// - The suite runner (`suite_runner.hpp`) measures every cell and repetition
//   in a fresh child process of the same executable (`cell_runner.hpp`), so
//   that no cell inherits another one's heap or cache state.
// - The binary fixes the allocator: `bbtree-bench` times cells on plain
//   mimalloc, and `bbtree-bench-memory` counts live bytes instead
//   (`memory.hpp`).
// - Some key kinds are read from real-world datasets (`datasets.hpp`), which
//   live outside the repository: `fetch` downloads them and `verify-data`
//   checks them.
#pragma once

#include "memory.hpp"

namespace bbtree_bench {

class bench_program {
 public:
  // Runs either binary's command line; returns the exit status. The memory
  // binary passes its counting allocator, whose live-byte count its cells
  // report.
  static int run(int argc, char** argv, run_mode mode);
};

}  // namespace bbtree_bench
