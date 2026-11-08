#!/usr/bin/env bash
# Makes the named LLVM tools (clang, clang++, clang-format) run version 15,
# the version the port is developed with, on a CI runner whose image ships a
# newer default. Registering version 15 as an alternative isn't enough: the
# newer one keeps a higher priority, silently. So this links the unversioned
# names to version 15 directly; the runner is thrown away after the job.
#
# Clang takes the libstdc++ of the newest GCC it finds, and Clang 15 can't
# parse GCC 14's C++20 ranges headers, which Catch2 and Abseil include. So
# GCC installations newer than 13 are moved out of its sight, leaving GCC
# 13's, the version the port is developed with; `g++` is GCC 13 either way.
set -euo pipefail
for tool in "$@"; do
  sudo ln -sf "/usr/bin/$tool-15" "/usr/bin/$tool"
  "$tool" --version | head -n 1
done
for install in /usr/lib/gcc/*/*; do
  version=$(basename "$install")
  if [[ "$version" =~ ^[0-9]+$ ]] && ((version > 13)); then
    triple=$(basename "$(dirname "$install")")
    sudo mkdir -p "/usr/lib/gcc-hidden/$triple"
    sudo mv "$install" "/usr/lib/gcc-hidden/$triple/$version"
  fi
done
clang++ -v -x c++ /dev/null -fsyntax-only 2>&1 | grep 'Selected GCC installation'
