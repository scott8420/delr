#!/usr/bin/env bash
# Configure + build. Passes any extra args through to cmake --build.
# The core's checks are NOT run here -- invoke them when you want them:
#   ./build/delr --selftest
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}"
cmake --build build -j"$(nproc)" "$@"
echo
echo "built: build/delr    (checks: ./build/delr --selftest)"
