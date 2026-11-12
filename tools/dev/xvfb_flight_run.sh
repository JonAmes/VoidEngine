#!/usr/bin/env bash
# Checklist 7.16.2 — proof 2 of the Phase 7 renderer (#47c): the scripted
# takeoff -> cruise -> capture -> pad ride from tests/core/flight_scenario.h
# played through the WSI presenter for human eyes, under Xvfb + lavapipe —
# the same reproducible backend decision #36 established for the spin demo.
# Not part of ci_local.sh: this needs a display (a virtual one) by design, so
# it lives beside the evidence it produces. Run it and commit what it writes.
#
# The demo's exit code is the gate (the 2.9 pattern): non-zero when the flight
# did not complete, when a capture probe shows the hull unpainted (<150 px, the
# 7.15 floor), when first/last frames are identical (poses not reaching the
# image), when fps falls off the floor, or when the validation layer says
# anything at all — and this run is the LIVE attestation of the 7.16.1 WSI
# depth attachment (ctest only shape-pins that seam).
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${BUILD_DIR:-build}"
OUT="${EVIDENCE_DIR:-docs/phase-07-atmosphere-contacts/evidence}"
# stride 60 = one presented frame per sim second; 360 frames = the whole
# 21,600-tick flight (6 sim minutes).
FRAMES="${FRAMES:-360}"
STRIDE="${STRIDE:-60}"
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

echo "== 7.16.2 flight evidence: $FRAMES frame(s) at stride $STRIDE under Xvfb + lavapipe =="
VK_ICD_FILENAMES="$LVP_ICD" VK_DRIVER_FILES="$LVP_ICD" \
    xvfb-run -a -s "-screen 0 1280x720x24" \
    "$DEMO" --mode flight --frames "$FRAMES" --stride "$STRIDE" --out "$OUT"

echo "== wrote =="
ls -1 "$OUT"
echo "xvfb_flight_run: GREEN — commit $OUT (whole flight presented, hull painted, depth path validated)"
