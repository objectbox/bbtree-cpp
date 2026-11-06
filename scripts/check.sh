#!/usr/bin/env bash
# The local checks (AGENTS.md, Verification): formatting, warnings as errors
# under Clang and GCC, and the tests; with --sanitize, also under
# AddressSanitizer + UBSan, the local stand-in for the Rust crate's Miri.
set -euo pipefail
cd "$(dirname "$0")/.."

files=$(git ls-files '*.hpp' '*.cpp')
if [ -n "$files" ]; then
  # shellcheck disable=SC2086
  clang-format --dry-run --Werror $files
fi

presets=(debug gcc)
if [ "${1:-}" = "--sanitize" ]; then
  presets+=(asan)
fi
for preset in "${presets[@]}"; do
  cmake --preset "$preset" >/dev/null
  cmake --build --preset "$preset"
  ctest --preset "$preset" -j "$(nproc)"
done
