#!/usr/bin/env bash
# Checklist 8.18 — the HUMAN-EYES half of "land + zoom render proof": the
# streamed real-scale descent (chunks + morph skirts + ONE log-zoom reversed-Z
# projection, 20 km -> 1.5 km) played through the WSI presenter under Xvfb +
# lavapipe. The hard proof (byte-exact goldens, exact depth pins, bit-identical
# frames across the first lattice flip) is the terrain_descent_golden CTest
# case inside ci_local.sh; this script is the watched version, like
# xvfb_flight_run.sh was for 7.16.2. Not part of ci_local.sh (display by design).
#
# The demo's exit code is the gate (the 2.9 pattern): non-zero when any frame
# painted under 0.90 (the blue clear = chunk-selection hole / swap-gap probe),
# when a PNG write failed, or when the window closed early — plus any
# validation-layer complaint fails the run like every other Xvfb evidence pass.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${BUILD_DIR:-build}"
OUT="${EVIDENCE_DIR:-docs/phase-08-scale-warp-streaming/evidence}"
# 12 poses, geometric 20 km -> 1.5 km (more time low, where streaming churns).
FRAMES="${FRAMES:-12}"
DEMO="$BUILD_DIR/tools/demo/ve_demo"

missing=0
for t in xvfb-run glslangValidator; do
    command -v "$t" >/dev/null 2>&1 || { echo "MISSING: $t (apt install xvfb glslang-tools)" >&2; missing=1; }
done
[ -x "$DEMO" ] || { echo "MISSING: $DEMO — build first (cmake --build $BUILD_DIR)" >&2; missing=1; }

LVP_ICD="${LVP_ICD:-}"
if [ -z "$LVP_ICD" ]; then
    for cand in /usr/share/vulkan/icd.d/lvp_icd*.json /etc/vulkan/icd.d/lvp_icd*.json; do
        [ -f "$cand" ] && { LVP_ICD="$cand"; break; }
    done
fi
[ -n "$LVP_ICD" ] && [ -f "$LVP_ICD" ] || { echo "MISSING: lavapipe ICD manifest lvp_icd*.json (apt install mesa-vulkan-drivers)" >&2; missing=1; }

layer="$(find /usr/share/vulkan/explicit_layer.d /usr/share/vulkan/implicit_layer.d \
             /etc/vulkan/explicit_layer.d /etc/vulkan/implicit_layer.d \
             -iname 'VkLayer_khronos_validation.json' 2>/dev/null | head -n1 || true)"
[ -n "$layer" ] || { echo "MISSING: VK_LAYER_KHRONOS_validation manifest (apt install vulkan-validationlayers)" >&2; missing=1; }
[ "$missing" -eq 0 ] || exit 1

mkdir -p "$OUT"

echo "== 8.18 descent evidence: $FRAMES poses, 20 km -> 1.5 km, Xvfb + lavapipe =="
VK_ICD_FILENAMES="$LVP_ICD" VK_DRIVER_FILES="$LVP_ICD" \
    xvfb-run -a -s "-screen 0 1280x720x24" \
    "$DEMO" --mode descent --frames "$FRAMES" --out "$OUT"

echo "== wrote =="
ls -1 "$OUT"
echo "xvfb_descent_run: GREEN — commit $OUT (streamed descent presented; one projection end to end; blue-hole probe under floor)"
