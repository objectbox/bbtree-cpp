#!/usr/bin/env bash
# The local checks (AGENTS.md, Verification): formatting, warnings as errors
# under Clang and GCC, and the tests; with --sanitize, also under
# AddressSanitizer + UBSan, the local stand-in for the Rust crate's Miri.
set -euo pipefail
cd "$(dirname "$0")/.."

# clang-format at one version on every machine, as two versions format some
# code differently: PyPI's build, which uvx (https://docs.astral.sh/uv/)
# fetches once, 2 MB. Reformat with the same command and `-i`.
clang_format=(uvx --from clang-format==20.1.8 clang-format)
files=$(git ls-files '*.hpp' '*.cpp')
if [ -n "$files" ]; then
  "${clang_format[@]}" --version
  # shellcheck disable=SC2086
  "${clang_format[@]}" --dry-run --Werror $files
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
