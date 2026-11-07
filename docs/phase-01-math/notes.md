# Phase 1 — notes

## Decisions & findings
- **Decision #34 (user, 2026-11-07):** world frame = right-handed, Y-up, −Z forward. Consequences recorded in PLAN.md decision #34: GLB pass-through is identity (assert the glTF Y-up invariant instead of rotating), OpenXR path needs no basis transform, euler convention = yaw(Y) → pitch(X) → roll(Z) right-handed.
- Epsilon semantics for `approximately_equal`: absolute eps below |x|≈1, relative eps beyond — documented at 1.1, every comparison test names which regime it exercises.

## Gotchas (learned across 1.1–1.7)
- Hand-derived clip-space pins are where the bugs live, not the matrices. Three separate derivation slips (look_at view pin, Vulkan depth curve, off-axis B sign) — all test-side. Mitigation that worked: compile a tiny probe binary that prints the actual matrix/clip values, derive on paper, only then write the CHECK.
- Vulkan NDC depth for finite far is `ndc.z = f(d−n) / (d(f−n))` — NOT the GL-era `1−n/d`-style curve. Sign of `1/(n−f)` z-row fooled two comments before a pin caught it.
- Off-axis: with L/R/T/B as *tangents*, `A = 2/(r−l)` carries no near factor, and `r−l > 0` always — `B = (r+l)/(r−l)` sign comes out of that ordering, not intuition.
- `to_model_matrix` is the fp32 render boundary: feeding a raw AU-scale position quantizes at ~16 km. Doctrine: camera-relative (floating-origin) locals only; the 1.7 origin test pins both the failure and the fix.
- doctest rejects "too complex" expressions: split multi-operand range comparisons into two CHECKs.
- Floating origin semantics matter: world positions stay ABSOLUTE fp64 (replay identity); only cached LOCALS translate by the shift delta. A "rewrite all positions" variant looks fine and silently breaks replay hashes.
- Inverse-matrix indexing (1.4): column-major row-0 expansion pivots on m[0],m[4],m[8],m[12]; adjugate entry (r,c) uses the minor dropping row c, col r — transposed pair. Both errors are silent-wrong, not compile errors; the `M*inv == I` pin is the only cheap guard.
