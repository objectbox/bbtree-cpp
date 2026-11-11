# bbtree++ — agent and contributor rules

This is a C++20 port of bbtree (Rust). `design/` is the Rust original's
design, which this port follows; PORTING.md says where and why it differs.

## Write code that requires minimal human attention 

- Fact: every line of code requires human review, so write readable code not just working code.

- Build reusable, trustworthy components that has deep logic but thin interface, so that human only need to review the complexity once (reuse the attention).

- Follow modern C++ best practices (C++ Core Guidelines: RAII, no owning raw pointers outside the node code, `noexcept` where the contract says no throw, `[[nodiscard]]`) to save human attention from reviewing for common mistakes.

- Before implementing a new feature, identify similar existing features, and check if it's worth to refactor existing code so it's reusable.

## Engineering

- Prefer explicit props and function arguments so ownership and data flow remain visible. Use context only when a dependency must cross at least two component layers and the intermediate parent does not use it. Keep state owned by the component that creates it.


- use descriptive, intention-revealing names for variables and functions, don't use generic names like `Built`, `Common`, `View`.

- do not use free form functions, functions should have a clear owner, e.g., a class, struct or enum's traits struct; unless it is a fully justified stateless function.
  Example 1: `chart build_chart(const data_model& model)`, instead use a member function like `chart data_model::build_chart() const`. Even better, a constructor `chart::chart(const data_model&)`.
  Example 2: `void toggle_expand(code_state& code, std::pair<uint32_t, uint32_t> key)`, instead `void code_state::toggle_expand(std::pair<uint32_t, uint32_t> key)`.

- try to use minimal visibility for functions and variables: library internals live in `namespace bbtree::detail` (never part of the API), class members are private unless needed, and helpers used by one class are its private members.

- try to avoid public data members, prefer getters and setters, unless the member is truly public (plain aggregates such as `page_layout`).

- organize the code by features, not by technical buckets, e.g., don't have `api.hpp`, `views.hpp`, `data.hpp`, etc. Instead, do `settings.hpp`, `data_panel.hpp`, `code_map.hpp`, etc.

- No static (or global) mutable variables. `static constexpr` constants are fine. One exception, in a test build only: the thread's scheduler of the shared map's interleaving tests is a `thread_local` pointer (PORTING.md, [switch points](PORTING.md#p45-switch-points-in-the-latched-tree-and-a-scheduler-of-threads-for-tests)).

## Performance reasoning

- Reason then measure. Don't start with blind sweeping, or brainless micro/premature optimizations. Always first think about what is the bottleneck by profiling and measurements, reason the cost model. Benchmark is the last step, to verify that your reasoning is correct, if not, correct your model and repeat; benchmark is NOT a tool to guide the optimization.

- Never do explicit prefetching: no software prefetch instructions or intrinsics (`_mm_prefetch`, `__builtin_prefetch`, inline `prefetcht0`, ...), not in the library, benchmarks, prototypes or A/B variants, and not behind a feature flag. Fix memory latency with layout instead: put what an operation needs in the cache lines it already loads (design/03-decisions.md, "No software prefetch").
  Exception: the batch lanes (design/12-batch-api.md) prefetch, only through `prefetcher` in `bbtree/include/bbtree/detail/tree/batch.hpp`, the line each lane reads next, the query window included. Never in single-key paths, and never whole keys or unchosen edges.

- A hot loop that stores into a page keeps its state (lengths, cursors, the leaf it fills) in locals, not members: a store through `std::byte*` may alias any member, so the compiler reloads them after every store, which Rust's `&mut self` rules out (PORTING.md, [loops that store into a page](PORTING.md#p30-loops-that-store-into-a-page-keep-their-state-in-locals)).

## Verification

- Local checks are `scripts/check.sh` (needs [uv](https://docs.astral.sh/uv/) for its pinned clang-format): `clang-format --dry-run --Werror`, a warnings-as-errors build under Clang and GCC, and `ctest`. `scripts/check.sh --sanitize` adds AddressSanitizer + UBSan, the stand-in for the Rust crate's Miri.
- ThreadSanitizer (`--preset tsan`) runs the concurrent map's tests; see PORTING.md for the races it is told to ignore.
- The `asan`, `tsan` and `fuzz` presets build with assertions on. `BBTREE_CHECKED` compiles the structural checks (`check_invariants`); tests and fuzz targets set it, an installed bbtree never has it.
- Valgrind memcheck on the concurrent map's tests needs `--fair-sched=yes`: Valgrind runs one thread at a time, and under its default scheduler a spinning thread starves the others for hours.
- Any recent Clang or GCC builds the port; it was written and measured with Clang 15 and GCC 13, and CMake 3.28. CI (GitHub, `objectbox/bbtree-cpp`; `gh run view --log-failed` for a failed run) builds on Linux with Ubuntu 24.04's default Clang and on macOS with Homebrew's newest, so a newer Clang's warnings show up there first. clang-format is pinned (`scripts/check.sh`, through `uvx`), as two versions format differently. A job that fails before reaching the port's code most likely means the runner image changed.
- The Rust original builds with `cargo +1.99.0` where the default toolchain is older.

## Benchmark rule

- A benchmark is useless if it takes more than 5 minutes to run.
- Never run full benchmark sweep without explicit approval.
- It's meaningless to repeat a benchmark, if the run to run variance is high, either the machine is not stable -- then wait for it to stablize, or the benchmark/implementation is broken -- then fix it. Repeating and taking average/median is for losers to cover benchmark flaw.
- Only run a carefully selected subset of relevant benchmarks.
- Start a timing run only on a quiet machine: a 1-minute load under 0.3, checked every few seconds (an IDE indexing counts). Time a comparison of the port with the Rust crate in one window, both benches back to back.
- Cells that build many maps (`1Mx4`, `100kx32`) take about a GB of fresh huge pages; when the machine's memory is mostly page cache the kernel stalls in compaction. Read `compact_stall` in `/proc/vmstat` before and after such a cell: one that stalled is not a measurement.
- Two builds that run the same instructions can differ by 5-19% in cycles on a cell from code placement alone (`-falign-functions=64` doesn't remove it), and 1M-entry cells move 10-30% between runs even on a quiet machine. Before calling a gap a regression, compare instruction counts (`perf stat -e instructions,cycles` on the one cell); the bench's `--perf` needs an L1d counter that some machines lack, where `perf stat` per cell is the fallback.
- `bbtree-bench-memory` counts heap bytes, which don't depend on load: one run is exact. Its RSS columns move in 64 KiB steps, meaningless at 1k entries.
- A probe outside the bench builds as the bench does, or it measures something else: C++ with the bench preset's flags (`-O3 -march=native -DNDEBUG`, LTO, mimalloc), Rust with `-C target-cpu=native` (the Rust bench's `.cargo/config.toml`) and its release profile. A probe that times a cold one-shot cell must not rebuild on warm pages.

## Docs

- General docs (the READMEs, the bench's COMPARISON.md, COMPARISON_HISTORY.md and CPP_MAPS.md, FUTURE_WORK.md, AGENTS.md) don't cite numbered entries such as P16, D25 or M11: link the section by its name, e.g. PORTING.md, [iteration](PORTING.md#p16-for_each-beside-iterators-for-rusts-folds). PORTING.md, PLAN.md and `design/` own their numbers.
- Tables comparing the port with the Rust crate put the port first as `bbtree++`, name the original `bbtree (Rust)`, and bold the fastest (or smallest) figure of each row. String-key rows run bbtree++ with the compact string fastest on the cell and name it in the row.

## Git

- Agents commit but never push; the maintainer pushes, often a work-in-progress branch to check it against CI, which runs on every branch. Commit messages carry no attribution lines.
- Keep history straight: a fix to an unpushed commit is folded into it (`git commit --fixup` and an autosquash rebase), not added on top. Pushed `main` is never rewritten; a pushed feature branch only with the maintainer's OK, else new commits go on top. `main` advances by fast-forward to a finished feature branch, so fixes go on the branch, not onto `main`.
- Addressing a review: one commit per point.
