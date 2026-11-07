#!/usr/bin/env bash
# Makes the named LLVM tools (clang, clang++, clang-format) run version 15,
# the version the port is developed with, on a CI runner whose image ships a
# newer default. Registering version 15 as an alternative isn't enough: the
# newer one keeps a higher priority, silently. So this links the unversioned
# names to version 15 directly; the runner is thrown away after the job.
set -euo pipefail
for tool in "$@"; do
  sudo ln -sf "/usr/bin/$tool-15" "/usr/bin/$tool"
  "$tool" --version | head -n 1
done
