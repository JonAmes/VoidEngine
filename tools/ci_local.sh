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

# 2.11 preflight — the Tier-2 gfx goldens decode committed PNGs and force the
# lavapipe ICD (decision #35). Fail HERE, with the fix, instead of letting
# those tests self-skip and reporting a falsely-quiet GREEN on a box that was
# never really running the render gate. (Manifest names vary by Mesa packaging;
# same discovery style as tools/dev/xvfb_run.sh.)
lvp_found=
for d in /usr/share/vulkan/icd.d /etc/vulkan/icd.d; do
  for cand in "$d"/lvp_icd*.json; do
    [ -f "$cand" ] && { lvp_found=yes; break 2; }
  done
done
if [ -z "$lvp_found" ]; then
  echo "MISSING: lavapipe ICD (lvp_icd*.json) — the gfx golden tests would all skip." >&2
  echo "         Debian/Ubuntu: sudo apt install mesa-vulkan-drivers" >&2
  exit 1
fi
if ! find /usr/share/vulkan/explicit_layer.d /etc/vulkan/explicit_layer.d \
        -iname 'VkLayer_khronos_validation.json' 2>/dev/null | grep -qm1 .; then
  echo "MISSING: Khronos validation layer — required by the strict-messenger policy (2.7)." >&2
  echo "         Debian/Ubuntu: sudo apt install vulkan-validationlayers" >&2
  exit 1
fi

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
