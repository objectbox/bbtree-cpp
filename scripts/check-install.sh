#!/usr/bin/env bash
# The install check: installs bbtree++ into a prefix, moves the prefix, and
# builds and runs a consumer project (tests/installed) against the moved
# copy with `find_package(bbtree)`, as a user's project would.
set -euo pipefail
cd "$(dirname "$0")/.."

work=build/install-check
rm -rf "$work"
cmake -S . -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBBTREE_BUILD_TESTS=OFF -DBBTREE_INSTALL=ON >/dev/null
cmake --build "$work/build"
cmake --install "$work/build" --prefix "$work/prefix" >/dev/null
mv "$work/prefix" "$work/moved-prefix"
cmake -S tests/installed -B "$work/consumer" -G Ninja -DCMAKE_PREFIX_PATH="$PWD/$work/moved-prefix" >/dev/null
cmake --build "$work/consumer"
ctest --test-dir "$work/consumer" --output-on-failure
echo "install check: the moved install builds and runs a consumer"
