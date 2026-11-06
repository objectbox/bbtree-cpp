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

- No static (or global) mutable variables. `static constexpr` constants are fine.

## Performance reasoning

- Reason then measure. Don't start with blind sweeping, or brainless micro/premature optimizations. Always first think about what is the bottleneck by profiling and measurements, reason the cost model. Benchmark is the last step, to verify that your reasoning is correct, if not, correct your model and repeat; benchmark is NOT a tool to guide the optimization.

- Never do explicit prefetching: no software prefetch instructions or intrinsics (`_mm_prefetch`, `__builtin_prefetch`, inline `prefetcht0`, ...), not in the library, benchmarks, prototypes or A/B variants, and not behind a feature flag. Fix memory latency with layout instead: put what an operation needs in the cache lines it already loads (design/03-decisions.md, "No software prefetch").
  Exception: the batch lanes (design/12-batch-api.md) prefetch, only through `prefetcher` in `bbtree/include/bbtree/detail/tree/batch.hpp`, the line each lane reads next, the query window included. Never in single-key paths, and never whole keys or unchosen edges.

## Verification

- Local checks are `scripts/check.sh`: `clang-format --dry-run --Werror`, a warnings-as-errors build under Clang and GCC, and `ctest`. `scripts/check.sh --sanitize` adds AddressSanitizer + UBSan, the stand-in for the Rust crate's Miri.
- ThreadSanitizer (`--preset tsan`) runs the concurrent map's tests; see PORTING.md for the races it is told to ignore.

## Benchmark rule

- A benchmark is useless if it takes more than 5 minutes to run.
- Never run full benchmark sweep without explicit approval.
- It's meaningless to repeat a benchmark, if the run to run variance is high, either the machine is not stable -- then wait for it to stablize, or the benchmark/implementation is broken -- then fix it. Repeating and taking average/median is for losers to cover benchmark flaw.
- Only run a carefully selected subset of relevant benchmarks.
