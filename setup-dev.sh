#!/usr/bin/env bash
# Dev dependencies (Debian/Ubuntu). Checked in so the environment is
# reproducible rather than re-derived each session.
set -euo pipefail
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"
$SUDO apt-get update
$SUDO apt-get install -y \
    build-essential cmake pkg-config \
    libgtkmm-4.0-dev libspdlog-dev
echo
echo "installed:"
pkg-config --modversion gtkmm-4.0 | sed 's/^/  gtkmm-4.0 /'
pkg-config --modversion spdlog    | sed 's/^/  spdlog    /'
