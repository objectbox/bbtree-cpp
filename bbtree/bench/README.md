# bbtree++ bench

A port of the Rust crate's bench harness (`bbtree/bench` in
[bbtree](https://github.com/XiangpengHao/bbtree)): the same suites, cells,
key sets, orders, CSV columns and markdown report, so that a C++ run and a
Rust run of the same cells compare cell by cell. `bbtree::unsync::bbtree_map`
(`bbtree-unsync`) and `bbtree::bbtree_map` (`bbtree`, opt-in) run against
`std::map` (`std-btree`), `absl::btree_map` (`absl-btree`),
`ankerl::unordered_dense::map` (`hashbrown`) and `std::unordered_map`
(`std-hash`). PORTING.md says how the port differs and why: [the allocator](../../PORTING.md#p17-the-benchs-allocator-mimalloc-332-behind-operator-new),
[the maps](../../PORTING.md#p18-the-benchs-maps-under-the-rust-benchs-names) and [the inputs](../../PORTING.md#p19-bench-inputs-the-rust-benchs-key-for-key).

## Build

```bash
cmake --preset bench && cmake --build --preset bench
```

Clang, Release, `-march=native`, LTO. It fetches abseil 20250814.2,
unordered_dense 4.9.2 and mimalloc 3.3.2, pinned; the library itself has no
dependencies. Two binaries land in `build/bench/bbtree/bench/`:

- `bbtree-bench` times cells on mimalloc;
- `bbtree-bench-memory` reports the bytes the maps of build operations hold.

## Run

Every cell (map x key x value x size x operation) runs in a fresh child
process, once per repetition, and the run writes a CSV and a markdown summary
to `bbtree/bench/results/` (or `--out DIR`).

```bash
B=build/bench/bbtree/bench
$B/bbtree-bench --help
$B/bbtree-bench ci --quick --key u64 --op get_hit,random_insert --list   # what would run
$B/bbtree-bench ci --quick --key u64 --op get_hit,random_insert         # under a second
$B/bbtree-bench small --quick --key u64 --size 1k                       # every one-map operation
$B/bbtree-bench tiny --quick --size 1Mx4                                # the many-map operations
$B/bbtree-bench-memory ci --quick --key u64 --size 1M --op random_insert
```

Suites: `ci`, `small`, `tiny`, `realkeys`, or `all`. Filters (`--map`,
`--key`, `--value`, `--size`, `--op`) take comma-separated names; `--reps`,
`--quick` (10x smaller), `--perf` (perf counters of the timed regions) and
`--list` work as in the Rust bench. A full suite at full size takes minutes
to tens of minutes: run filtered, quick subsets unless you mean it.

`--perf` needs every event of the Rust bench's group; on Zen 3 neither bench
gets `L1-dcache-load-misses:u` and both refuse with the same message.

## Datasets

`realkeys`, `tiny`'s `1Mxreal` and `small`'s crate names and kernel paths
read datasets from `$BBTREE_BENCH_DATA`, or `$XDG_CACHE_HOME/bbtree-bench`,
or `~/.cache/bbtree-bench`, the Rust bench's directory: one download serves
both benches. `bbtree-bench fetch` downloads exactly what the Rust bench's
does (`gh release download bench-data-2026-10-08 --repo XiangpengHao/bbtree`,
`zstd`, and `tools/bbtree_datasets.py cc-urls` for Common Crawl's URLs);
`verify-data` hashes them against the manifest. Cells whose files are missing
are listed and recorded as `missing-data`, and the rest run.

## Compare against the Rust bench

Run the same cells in both, then `compare`, from either bench, reads either
CSV:

```bash
$B/bbtree-bench ci --quick --key u64 --op get_hit,random_insert --out /tmp/cpp
(cd path/to/bbtree/bbtree/bench && cargo run --release -- ci --quick --key u64 --op get_hit,random_insert --out /tmp/rust)
$B/bbtree-bench compare --head /tmp/cpp/<stamp>-timing.csv --base /tmp/rust/<stamp>-timing.csv
```

With the C++ run as head and the Rust run as base, the first table is
`bbtree-unsync`'s change from Rust to C++, cell by cell; the second is each
head's `bbtree-unsync` against its own `std-btree` (`std::map` in C++,
`BTreeMap` in Rust).

## Checking the keys against the Rust bench

`bbtree-bench self-check` digests 50 sequences that seven cells use (key
sets, insertion, probe, removal and upsert orders, many-map picks and
misses, a key list's samples, struct values) and compares them with digests
made by the Rust bench's own code. To remake those: copy the Rust bench's
`Cargo.toml`, `Cargo.lock`, `src/` and `.cargo/` to a scratch directory,
point its `bbtree` dependency at the Rust crate, make
`operations::{single_map, many_maps}` and `datasets::key_list` `pub(crate)`,
and add to each a `digest` module that prints, for the cells `self_check.cpp`
lists, `<label> <count> <FNV-1a 64 of the sequence>` (integers as 8
little-endian bytes, strings as their length, so encoded, then their bytes); then
compare its output with the C++ check's lines.

## Layout

One file per feature, after the Rust bench's modules: `command_line`,
`suites`, `workload`, `keys`, `values`, `random`, `single_map`, `many_maps`,
`map_adapters` and `map_kind`, `cell_runner` (the child), `suite_runner`
(the parent), `report`, `run_comparison`, `perf_counters`, `datasets`,
`key_list`, `json_objects`, `dataset_fetch`, `memory`, and the two binaries'
`timing_main` and `memory_main`.
