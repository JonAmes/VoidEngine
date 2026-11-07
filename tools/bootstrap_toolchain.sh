#!/usr/bin/env bash
# Phase 0.1 — reprovision the build toolchain WITHOUT system sudo.
# Installs cmake + ninja as user-local official binaries under ~/.local.
# Idempotent: re-running detects existing installs and does nothing.
# Rationale: no interactive sudo on the CI box; apt-level cmake/ninja need it,
# these are the upstream official releases (checksum-pinned). C++ compiler
# (g++ >= 14, C++23) is expected from the distro; everything else arrives via
# CMake FetchContent at configure time.
set -euo pipefail

PREFIX="${HOME}/.local"
BIN="${PREFIX}/bin"
OPT="${PREFIX}/opt/void-toolchain"
mkdir -p "$BIN" "$OPT"

CMAKE_VERSION="3.31.6"
NINJA_VERSION="1.12.1"
# sha256 of the official release archives (pinned — fail loud on mismatch)
CMAKE_SHA256="5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf"
NINJA_SHA256="6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255"

need() { command -v "$1" >/dev/null 2>&1 && "$1" --version >/dev/null 2>&1; }

fetch() { # url dest sha256
  curl -fL --retry 3 -o "$2" "$1"
  echo "$3  $2" | sha256sum -c - || { echo "CHECKSUM MISMATCH for $2 — refusing"; exit 1; }
}

if ! need cmake; then
  echo "installing cmake ${CMAKE_VERSION} -> ${OPT}"
  fetch "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
    "${OPT}/cmake.tar.gz" "$CMAKE_SHA256"
  mkdir -p "${OPT}/cmake"
  tar -xzf "${OPT}/cmake.tar.gz" -C "${OPT}/cmake" --strip-components=1
  rm -f "${OPT}/cmake.tar.gz"
  ln -sf "${OPT}/cmake/bin/cmake" "${BIN}/cmake"
  ln -sf "${OPT}/cmake/bin/ctest" "${BIN}/ctest"
  ln -sf "${OPT}/cmake/bin/cpack" "${BIN}/cpack"
fi

if ! need ninja; then
  echo "installing ninja ${NINJA_VERSION} -> ${OPT}"
  fetch "https://github.com/ninja-build/ninja/releases/download/v${NINJA_VERSION}/ninja-linux.zip" \
    "${OPT}/ninja.zip" "$NINJA_SHA256"
  python3 - "$OPT/ninja.zip" "$BIN/ninja" <<'PY'
import sys, zipfile, os, stat
with zipfile.ZipFile(sys.argv[1]) as z:
    data = z.read("ninja")
with open(sys.argv[2], "wb") as f:
    f.write(data)
os.chmod(sys.argv[2], os.stat(sys.argv[2]).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
PY
  rm -f "${OPT}/ninja.zip"
fi

echo "cmake: $(cmake --version | head -1)"
echo "ninja: $(ninja --version)"
echo "ctest: $(ctest --version | head -1)"
echo "OK — toolchain ready (user-local, no sudo)"
