#!/usr/bin/env bash
# Editor kit EK.4 (decision #54) — the ve_edit evidence run, same shape as
# 2.9's xvfb_run.sh: Xvfb + lavapipe, NEVER part of ci_local (it needs a
# display by design). ve_edit self-attests with exit codes (see
# tools/edit/src/main.cpp's header contract); this script only supplies the
# virtual display + ICD preflight and commits what ve_edit writes.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${BUILD_DIR:-build}"
OUT="${EVIDENCE_DIR:-docs/tooling-editorkit/evidence}"
FRAMES="${FRAMES:-16}"
EDIT="$BUILD_DIR/tools/edit/ve_edit"

missing=0
for t in xvfb-run glslangValidator; do
    command -v "$t" >/dev/null 2>&1 || { echo "MISSING: $t (apt install xvfb glslang-tools)" >&2; missing=1; }
done
[ -x "$EDIT" ] || { echo "MISSING: $EDIT — build first (cmake --build $BUILD_DIR)" >&2; missing=1; }

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

echo "== EK.4 evidence: $FRAMES scripted frame(s) of ve_edit under Xvfb + lavapipe =="
VK_ICD_FILENAMES="$LVP_ICD" VK_DRIVER_FILES="$LVP_ICD" \
    xvfb-run -a -s "-screen 0 1600x1000x24" \
    "$EDIT" --frames "$FRAMES" --out "$OUT"

# ve_edit exits non-zero when the scripted form edit never reached the gate,
# the gate refused it, the edited name never hit disk, the UI drew no ink, the
# final frame equals frame 0, or the validation layer said anything (2.7
# policy, app-side) — so exit 0 plus the written PNGs IS the evidence.
echo "== wrote =="
ls -1 "$OUT"
echo "xvfb_edit_run: GREEN — forms edit round-tripped the real cook gate on an empty world"
