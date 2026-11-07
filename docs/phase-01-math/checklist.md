# Phase 1 — Math & the headless core
Status: in progress

World frame pinned by **decision #34: right-handed, Y-up, −Z forward** (`+X` right, `+Y` up, `−Z` forward, cross(X,Y)=Z). Matches glTF and OpenXR natively — every test below asserts *that* convention; a convention drift later = these tests fail, which is the point. fp32 (`vec*`/`mat4`/`quat`) is for rendering math + stats; **fp64 (`dvec3`) is sim positions/velocities** (decision #3).

## Tasks — execute top to bottom, one commit each
- [x] 1.1 `core/math` base: constants (pi/tau/inf), deg↔rad, `wrap_angle` to [-π,π), approximate-equal with documented eps semantics (absolute for small, relative for large) — test: `math_scalars`, `math_angles` — commit: 1df32e5
- [ ] 1.2 fp32 vectors `vec2f/vec3f/vec4f`: component ops, dot/cross/length²/length/normalize (zero-safe), lerp, projection/rejection; **decision-#34 convention pinned** — test: `math_vec3_ops`, `math_convention_handedness` (cross(X,Y)=Z, forward=-Z, right-handed rotation sense) — commit: —
- [ ] 1.3 fp64 `dvec3` (+`dvec2` as needed): full op set mirroring vec3f; precision proof at system scale (decision #22 numbers: AU-scale separations resolving cm/mm steps) — test: `math_dvec3_ops`, `math_fp64_precision` — commit: —
- [ ] 1.4 `mat4f`: multiply, transpose, point-vs-vector transform (w semantics), general inverse + fast rigid-body inverse (assert rigid precondition), trs constructors (column-major, right-handed) — test: `math_mat4_ops`, `math_mat4_inverse` — commit: —
- [ ] 1.5 `quatf`: axis-angle, from_signed_axis_angle, multiply (composition order documented + tested non-commutative), conjugate/inverse/normalize, rotate(vec), slerp (nlerp short-path), quat↔mat4, euler( yaw-Y, pitch-X, roll-Z per #34)↔quat round-trips; `look_at_rh` frame builder; TRS decompose — test: `math_quat_roundtrip`, `math_quat_euler`, `math_frame_change` — commit: —
- [ ] 1.6 Projection builders (the VR-critical task): symmetric perspective, ortho, and **off-axis asymmetric from explicit L/R/T/B tans + near/far** (AGENTS VR rule — no symmetric-only shortcut); Vulkan depth [0,1] mapping + reversed-Z variant; clip-space convention tests with hand-derived corner coords — test: `proj_symmetric`, `proj_ortho`, `proj_offaxis` — commit: —
- [ ] 1.7 Coordinate utilities: world↔local via trs (round-trip), spherical↔cartesian (polar singularity handled), dvec3 **floating-origin recenter** helper (origin shift preserves deltas; positions stay fp64) — test: `math_coord_conversions`, `math_origin_shift` — commit: —

## Done-when (every PLAN.md "Done when" bullet linked to a task; all checked = phase DONE)
- [ ] (a) 100+ math assertions pass headless ← 1.1–1.7 (count in final CI)
- [ ] (b) Projection math supports off-axis asymmetric frustums from day one ← 1.6
- [ ] (c) fp64 position types exist and cannot be "retrofitted later" — precision proven at real-system scale ← 1.3
- [ ] (d) Right-handed / Y-up / −Z-forward frame pinned by test (decision #34) ← 1.2

## Notes
- Hand-derived expected values in tests get their derivation in a comment (AGENTS comment rule).
- Header-only `core/math` if it stays cheap (templates/inlining); otherwise split — decide at 1.4, record here.
