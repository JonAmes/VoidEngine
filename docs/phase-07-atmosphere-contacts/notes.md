# Phase 7 — Notes

Design notes, findings, and gotchas worth keeping. One section per task number as work lands.

## 7.1 — body spin (sim_spin, 3058e68)
- Orientation is **analytic like rails**: no stored state, pure function of (def, tick), evaluable at any tick — scrub/replay-safe by construction.
- Composition: the spin axis is authored in the **parent rail frame**, so local spin RIGHT-multiplies the parent's world attitude: `orientation(id) = orientation(parent) ⊗ axisangle(axis, ω·t)`; expanded root-down. A non-spinning child inherits the parent's attitude bit-exactly (moon test).
- `add_body` normalizes the axis **once at add** (never per eval → bit-stable evals) and rejects NaN/inf spin fields and zero-axis + nonzero-rate (returns `nullopt`; core is exception-free).
- No pre-wrapping of the spin angle (same discipline as `local_rail_offset`): unit-length pinned at tick 864'000 (10 days at 1 rev/s, angle ~5.4e6 rad).
- Load-bearing invariant: `position()`/`velocity()` **never read spin** — pinned bit-exact between a spin-free and a spun world at five ticks. Wells/safe bubbles stay position-only ("centers move, normals don't", #41); pad tangential velocity later comes from the spin, not the rails.

## 7.2 — analytic terrain sampler (terrain_sampler, 2bce786)
- **Ambient 3D lattice on the cube-face chart, not per-face 2D noise.** The gnomonic projection p = dir/|dominant coord| is *continuous* across face switches (the projected 3D point agrees from both sides), so one shared integer lattice sampled at p is seam-free. Per-face 2D lattices would seam O(relief) at every face border — the seam test (44.9° vs 45.1° about the diagonal) pins this: |Δh| < 12 m where a seam would be O(100).
- **Hash chain**: lattice corner value = splitmix64(seed + x·G1 + y·G2 + z·G3) → h>>11 / 2^53 scaled to [-1,1). Integer-only mixing (bit-deterministic everywhere), distinct odd multipliers keep the coordinate combiner injective-ish for small offsets. Quintic smootherstep (C2) ⇒ height AND slope continuous across lattice cells — required for sane normals.
- **Hand pins are derivable, not recorded**: with base_frequency=1/lacunarity=2 the axis dirs (1,0,0),(0,1,0),(0,0,-1) land on EXACT lattice corners at all three octaves, so the 1-octave heights are single-multiply exact (== pin, no epsilon); 3-octave heights and the gradient normal came from an independent Python float port of the same arithmetic. FMA contraction + libm move multi-octave sums by ~1ulp/op → those pins carry epsilon 1e-12/1e-9, NOT bit-exact (contradicts nothing in #40: bit-stability is same-platform-same-code, not cross-platform bits).
- **Normal**: surface r(θ) = (R + h(θ))·dir ⇒ n ∝ (R+h)·d − (∂h/∂θ₁)e₁ − (∂h/∂θ₂)e₂; tangential derivatives via central differences at FIXED angular step 1e-5 rad (pure functions in, pure out). Quintic slope is 0 at lattice corners → the +X-axis normal is radial to ~1e-9 — doctest::Approx has no margin(); tiny tilt components need explicit fabs windows, and near-zero differences ride libm/FMA noise (epsilon on them fails).
- **face_uv semantics**: u,v = raw ratios of non-dominant coords over |dominant| (sign of the dominant coord lives in the face code). Reconstruction round-trips: p = (±1, u, v) placed on the face. Keep this for 7.3 stamp/pad coords.
- **Scale invariance is ulp-level, not bit-level**: normalize(2000,9000,-3000) ≠ normalize(0.2,0.9,-0.3) by a few ulp (division rounds), so pre-normalize equality only holds within epsilon. Purity pins (same args → same bits) stay bit-exact.
- Degenerate inputs pinned, never thrown: zero dir ≡ +Y; octaves clamped to [1, 8]; lattice coords stay ≪ 2^63 by the octaves/frequency caps (comment in terrain.cpp).
- #41 hookup: sampler takes a **body-local direction**; world→local goes through `orientation(id, t)` from 7.1, so the surface rides spin for free and the sampler itself never sees a tick.
- Trap for 7.3+: height is measured ABOVE mean `surface_radius` (relief rides the sphere); `terrain_normal` needs `mean_radius_m` — contacts must pass the body's authored radius, not a constant.

