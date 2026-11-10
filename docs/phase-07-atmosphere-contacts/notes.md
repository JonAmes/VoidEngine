# Phase 7 — Notes

Design notes, findings, and gotchas worth keeping. One section per task number as work lands.

## 7.1 — body spin (sim_spin, 3058e68)
- Orientation is **analytic like rails**: no stored state, pure function of (def, tick), evaluable at any tick — scrub/replay-safe by construction.
- Composition: the spin axis is authored in the **parent rail frame**, so local spin RIGHT-multiplies the parent's world attitude: `orientation(id) = orientation(parent) ⊗ axisangle(axis, ω·t)`; expanded root-down. A non-spinning child inherits the parent's attitude bit-exactly (moon test).
- `add_body` normalizes the axis **once at add** (never per eval → bit-stable evals) and rejects NaN/inf spin fields and zero-axis + nonzero-rate (returns `nullopt`; core is exception-free).
- No pre-wrapping of the spin angle (same discipline as `local_rail_offset`): unit-length pinned at tick 864'000 (10 days at 1 rev/s, angle ~5.4e6 rad).
- Load-bearing invariant: `position()`/`velocity()` **never read spin** — pinned bit-exact between a spin-free and a spun world at five ticks. Wells/safe bubbles stay position-only ("centers move, normals don't", #41); pad tangential velocity later comes from the spin, not the rails.

