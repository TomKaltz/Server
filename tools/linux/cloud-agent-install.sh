#!/usr/bin/env bash
#
# Idempotent bootstrap for Cursor Cloud Agent development environments.
# Installs the CasparCG Server build toolchain and system dependencies,
# then performs a full CMake/Ninja build using the recommended system CEF
# path (ppa:casparcg/ppa). Safe to run repeatedly.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

export DEBIAN_FRONTEND=noninteractive

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

echo "==> Updating apt package index"
$SUDO apt-get update -yq

echo "==> Installing base build tooling"
$SUDO apt-get install -yq --no-install-recommends \
    software-properties-common \
    ca-certificates \
    ninja-build \
    cmake \
    libicu-dev \
    libjpeg-dev

echo "==> Installing CasparCG system dependencies"
$SUDO ./tools/linux/install-dependencies

echo "==> Enabling casparcg PPA (system CEF)"
$SUDO add-apt-repository -y ppa:casparcg/ppa
$SUDO apt-get update -yq

echo "==> Installing CEF development package"
$SUDO apt-get install -yq --no-install-recommends casparcg-cef-131-dev

# The default base image points the `c++`/`cc` alternatives at clang, whose
# GCC-toolchain autodetection picks a gcc dir without libstdc++ dev files and
# fails to link (-lstdc++). CasparCG is built with GCC upstream, so pin it here.
export CC=gcc
export CXX=g++

echo "==> Configuring build (system CEF, GCC toolchain)"
mkdir -p build
# Drop a stale cache that was configured with a different compiler.
if [ -f build/CMakeCache.txt ] && ! grep -q "CMAKE_CXX_COMPILER:.*g++" build/CMakeCache.txt; then
    echo "    stale CMake cache detected, clearing"
    rm -rf build
    mkdir -p build
fi
# libavdevice pulls in libcaca, which needs libncursesw symbols. Modern ld
# defaults to --no-copy-dt-needed-entries, so those transitive symbols are not
# resolved when linking the final executable; re-enable copying them.
cmake -GNinja -S src -B build \
    -DUSE_SYSTEM_CEF=ON \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--copy-dt-needed-entries"

echo "==> Building CasparCG Server"
cmake --build build --parallel "$(nproc)"

echo "==> Build complete: $REPO_ROOT/build/shell"
