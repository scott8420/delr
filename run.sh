#!/usr/bin/env bash
# Build (which runs the core's checks), then start the app.
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
echo
exec ./build/delr "$@"
