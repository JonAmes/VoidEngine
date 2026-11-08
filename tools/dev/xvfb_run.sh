#!/usr/bin/env bash
# Checklist 2.9 — the windowed evidence run, the way decision #36 wants it:
# Xvfb + the lavapipe ICD, never a workstation GPU. A "the triangle spins" claim
# that only survives on one vendor's display is not evidence, so the runner that
# the checklist commits is the reproducible one.
#
# Not part of ci_local.sh: this needs a display (a virtual one) by design, so it
# lives beside the evidence it produces. Run it and commit what it writes.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${BUILD_DIR:-build}"
OUT="${EVIDENCE_DIR:-docs/phase-02-vulkan/evidence}"
FRAMES="${FRAMES:-120}"
DEMO="$BUILD_DIR/tools/demo/ve_demo"

# Preflight with names, not mystery failures at vkCreateInstance time.
missing=0
for t in xvfb-run glslangValidator; do
    command -v "$t" >/dev/null 2>&1 || { echo "MISSING: $t (apt install xvfb glslang-tools)" >&2; missing=1; }
done
[ -x "$DEMO" ] || { echo "MISSING: $DEMO — build first (cmake --build $BUILD_DIR)" >&2; missing=1; }

LVP_ICD="${LVP_ICD:-/usr/share/vulkan/icd.d/lvp_icd.$(uname -m).json}"
[ -f "$LVP_ICD" ] || { echo "MISSING: lavapipe ICD at $LVP_ICD (apt install mesa-vulkan-drivers)" >&2; missing=1; }

# instance::create refuses any instance without the Khronos layer (2.2 policy),
# so a box without it cannot produce this evidence at all.
layer="$(ls /usr/share/vulkan/explicit_layer.d/VkLayer_KHRONOS_validation.json \
            /usr/share/vulkan/implicit_layer.d/VkLayer_KHRONOS_validation.json 2>/dev/null | head -n1 || true)"
[ -n "$layer" ] || { echo "MISSING: VK_LAYER_KHRONOS_validation manifest (apt install vulkan-validationlayers)" >&2; missing=1; }
[ "$missing" -eq 0 ] || exit 1

mkdir -p "$OUT"

# The engine enables the validation layer explicitly; forcing the ICD via BOTH
# spellings is what the gfx test fixtures do (#35) — older loaders read
# VK_ICD_FILENAMES, newer ones VK_DRIVER_FILES.
echo "== 2.9 evidence: $FRAMES frame(s) under Xvfb + lavapipe =="
VK_ICD_FILENAMES="$LVP_ICD" VK_DRIVER_FILES="$LVP_ICD" \
    xvfb-run -a -s "-screen 0 1280x720x24" \
    "$DEMO" --frames "$FRAMES" --out "$OUT"

# The demo exits non-zero when frames do not differ, when the swapchain never
# rebuilt after a resize request, or when the validation layer said anything at
# all — so a written summary.txt plus exit 0 IS the done-when evidence.
echo "== wrote =="
ls -1 "$OUT"
echo "xvfb_run: GREEN — commit $OUT (frames differ, fps stable, validation silent)"
