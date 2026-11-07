#!/usr/bin/env bash
# Phase 0.4 — the headless contract in one command (AGENTS.md: ctest must be
# green with no $DISPLAY, no compositor, no headset). Configure + build + ctest
# with the display stripped from the environment, so any test that secretly
# needs one fails HERE, not on someone's desktop.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"

for t in cmake ninja ctest; do
  command -v "$t" >/dev/null 2>&1 || {
    echo "MISSING: $t — run tools/bootstrap_toolchain.sh first" >&2
    exit 1
  }
done

echo "== configure (headless) =="
env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE \
  cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}"

echo "== build (headless) =="
env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE \
  cmake --build "$BUILD_DIR"

echo "== ctest (headless) =="
env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE \
  ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "ci_local: GREEN (no display, no compositor, no headset)"
