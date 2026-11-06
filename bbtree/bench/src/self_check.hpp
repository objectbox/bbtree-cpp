// `self-check`: checks that the generators make the Rust bench's keys,
// values and orders. Each sequence a few cells use, in full, is digested and
// compared with the digest the Rust bench's own code gives
// (`self_check.cpp` says how those were made), so that a C++ run and a Rust
// run of a cell measure the same key set in the same orders.
#pragma once

namespace bbtree_bench {

class self_check {
 public:
  // Prints every check; throws if any sequence differs from the Rust
  // bench's.
  static void run();
};

}  // namespace bbtree_bench
