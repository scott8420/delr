#!/usr/bin/env bash
# Dev dependencies. Checked in so the environment is reproducible rather than
# re-derived each session.
#
# It speaks apt AND dnf, because it used to speak only apt and that silently
# cost a session: `apt-get install libcurl4-openssl-dev` on a dnf box fails,
# `find_package(CURL QUIET)` then misses without a word, and the build goes
# green having quietly become a program that cannot fetch anything. A setup
# script that only works on one distro is not reproducing an environment, it is
# assuming one.
set -euo pipefail
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

if command -v apt-get > /dev/null 2>&1; then
    $SUDO apt-get update
    $SUDO apt-get install -y \
        build-essential cmake pkg-config \
        libgtkmm-4.0-dev libspdlog-dev \
        libcurl4-openssl-dev
elif command -v dnf > /dev/null 2>&1; then
    # -devel, not the bare runtime: find_package(CURL) needs curl/curl.h and
    # the pkg-config file, and `dnf install libcurl` provides neither.
    $SUDO dnf install -y \
        gcc-c++ cmake pkgconf-pkg-config \
        gtkmm4.0-devel spdlog-devel \
        libcurl-devel
else
    echo "No apt-get or dnf found. Install by hand:" >&2
    echo "  a C++17 compiler, cmake, pkg-config" >&2
    echo "  gtkmm-4.0, spdlog, libcurl -- all DEVELOPMENT packages" >&2
    exit 1
fi

echo
echo "installed:"
pkg-config --modversion gtkmm-4.0 | sed 's/^/  gtkmm-4.0 /'
pkg-config --modversion spdlog    | sed 's/^/  spdlog    /'
# The OpenSSL build on purpose: Apache-2.0, and the licence surface of a curl
# dependency is its TLS backend rather than curl itself. The GnuTLS build is
# LGPL and also fine to link -- it is bundling that would make the difference.
pkg-config --modversion libcurl   | sed 's/^/  libcurl   /'
echo
# The line that would have saved a session. libcurl is optional to the BUILD
# and mandatory to the PROGRAM: without it every check refuses, and the only
# place that was ever said out loud was one cmake status line scrolling past.
if ! pkg-config --exists libcurl; then
    echo "WARNING: no libcurl development package." >&2
    echo "  delr will build, and every check will refuse. Fix this first." >&2
    exit 1
fi
