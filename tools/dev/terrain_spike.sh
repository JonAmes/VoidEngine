#!/usr/bin/env bash
# Checklist 8.6 — terrain art-direction SPIKE (#17d, throwaway). Not part of
# ci_local.sh and not a ctest case BY DESIGN: the spike exists to produce the
# two looks for the USER decision gate on terrain art direction, and the
# checklist box says so. Re-run it any time; it rewrites its own evidence.
#
# Gate chain: ve_demo's own exit code (both looks painted >= 0.30, the looks
# differ >= 0.05, captures non-empty — exit 4 on any of them), then this
# script independently decodes the written PNGs (stdlib zlib) and re-checks
# the sky surround / painted center / look difference on the DISK pixels,
# so a capture bug cannot hide behind the demo's self-report.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="${BUILD_DIR:-build}"
OUT="${EVIDENCE_DIR:-docs/phase-08-scale-warp-streaming/evidence}"
FRAMES="${FRAMES:-4}"   # frames PER look (two looks render per run)
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

echo "== 8.6 terrain spike: $FRAMES frame(s) per look under Xvfb + lavapipe =="
VK_ICD_FILENAMES="$LVP_ICD" VK_DRIVER_FILES="$LVP_ICD" \
    xvfb-run -a -s "-screen 0 1440x810x24" \
    "$DEMO" --mode terrain --frames "$FRAMES" --width 1280 --height 720 --out "$OUT"

grep -q "PASS" "$OUT/terrain_summary.txt" || { echo "summary line did not PASS" >&2; exit 1; }

echo "== independent PNG decode cross-check (stdlib) =="
python3 - "$OUT/terrain_stylized.png" "$OUT/terrain_realistic.png" <<'PY'
import sys, zlib

def load_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', path + ': not a PNG'
    pos, idat, ihdr = 8, b'', None
    while pos < len(d):
        ln = int.from_bytes(d[pos:pos+4], 'big'); typ = d[pos+4:pos+8]
        body = d[pos+8:pos+8+ln]
        if typ == b'IHDR':
            ihdr = (int.from_bytes(body[0:4],'big'), int.from_bytes(body[4:8],'big'),
                    body[8], body[9])
        elif typ == b'IDAT':
            idat += body
        pos += 12 + ln
    w, h, depth, ctype = ihdr
    assert depth == 8 and ctype in (2, 6), f'{path}: unexpected color type {ctype}'
    nch = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    px = bytearray(w * h * nch)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 0: pass
        elif f == 1:
            for i in range(nch, stride): line[i] = (line[i] + line[i-nch]) & 255
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i-nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i-nch] if i >= nch else 0
                b = prev[i]; c = prev[i-nch] if i >= nch else 0
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = 0 if (pa <= pb and pa <= pc) else (1 if pb <= pc else 2)
                line[i] = (line[i] + (a if pr == 0 else (b if pr == 1 else c))) & 255
        else: raise AssertionError('bad filter ' + str(f))
        px[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, nch, px

def at(img, x, y):
    w, h, nch, px = img
    o = (y*w + x)*nch
    return px[o], px[o+1], px[o+2]

def frac_near(img, pred):
    w, h, nch, px = img
    n = sum(1 for i in range(0, len(px), nch) if pred(px[i], px[i+1], px[i+2]))
    return n / (w*h)

a = load_png(sys.argv[1]); b = load_png(sys.argv[2])
assert a[0] == b[0] and a[1] == b[1], 'size mismatch between looks'
w, h = a[0], a[1]
sky = lambda r, g, b_: abs(r-135) <= 12 and abs(g-171) <= 12 and abs(b_-209) <= 12
for img, name in ((a, 'stylized'), (b, 'realistic')):
    assert sky(*at(img, 4, 4)), name + ': top-left corner is not sky - camera framing wrong'
    assert not sky(*at(img, w//2, h-8)), name + ': bottom-center is sky - terrain not under the eye'
    painted = frac_near(img, lambda r, g, b_: not sky(r, g, b_))
    print(f'{name}: painted={painted:.3f}')
    assert painted >= 0.30, name + ': painted fraction under 0.30 on disk pixels'
diff_px = 0
for y in range(h):
    for x in range(w):
        ra, ga, ba = at(a, x, y); rb, gb, bb = at(b, x, y)
        if abs(ra-rb) > 8 or abs(ga-gb) > 8 or abs(ba-bb) > 8:
            diff_px += 1
print(f'look_diff={diff_px/(w*h):.3f} (disk pixels)')
assert diff_px/(w*h) >= 0.05, 'the two looks are identical on disk pixels'
print('PNG cross-check: PASS')
PY

echo "== wrote =="
ls -1 "$OUT"
echo "terrain_spike: GREEN — PNGs + summary written; the ART DECISION (#17d) is the user's, present these before 8.18 commits"
