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

## 7.3 — authored stamps (terrain_stamps, 5f26c9f)
- **Stack formula**: `h(dir) = h_base(dir) + (1 - W(dir)) * noise(dir)`, `W = max stamp weight`. Stamps compose over the FLAT baseline in array order (`hb = hb + w*(level - hb)`, later stamps blend over earlier results — order pinned by test); noise composes on top scaled by the worst suppression only. So a full-weight stamp pins h to its level with zero noise influence: structurally exact, not "small noise".
- **The pad guarantee (#29c) is a proof, not a tolerance**: `w == 1` uses a shortcut (`hb = level`, not the lerp — a lerp would round), and both central-difference probes land inside the same full-weight disk ⇒ D1 = D2 = 0 EXACTLY ⇒ normal is radial up to normalize rounding only. Different terrain seeds give the same exact pad level (noise-influence test).
- **quintic smootherstep does double duty**: plateau profile (full weight inside `(1-blend)·radius`, quintic ring out) and crater bowl (`quintic(1 - dd/radius)`: flat floor, wall to rim). quintic' = 0 at both ends ⇒ no crease at stamp edge OR at inner-ring edge (C2-ish everywhere). Gotcha: the ring's EDGES are near-radial normals — sample the steepest point (u=0.5) when asserting ring tilt.
- **`dd = acos(clamp(dot(d,c)))` puts libm in the sampler eval** (#40 contract covers it — same-platform determinism). dd jitter is ~ulp in ANGLE only near the center (acos'(1)=∞: 1e-15 dot error → ~6e-8 rad) — harmless vs inner radii ~1e-3, but it means no exact w=0.5 pins from geometry; mid-blend pins ride epsilon 1e-12.
- Degenerate stamps are skipped, never contaminate (bit-identical to unstamped, pinned): non-finite radius/level/blend, zero/NaN center (falls through `!(c2 > 0)`), radius ≤ 0. `blend ≤ 0` ⇒ hard disc (authored misuse; deterministic cut, normal undefined at rim by design — don't sample there); `blend > 1` clamps to 1.
- **Spin-frame contract deviation from checklist wording**: the sampler stays pose-free (local dirs only, never sees a tick — scrub/replay-safe by construction); stamps are authored body-local and RIDE SPIN because callers convert world↔local via `orientation()`/conjugate from 7.1. Test pins the round-trip: pad center sampled at tick 12'345 on a body spinning about +X comes back inside the full-weight disk with the exact level pin, and the world normal = q ⊗ local radial.
- Refactor discipline: 7.2's noise body moved verbatim into `noise_field()` — operation order is the 7.2 pinned bit-contract; `terrain_sampler`'s exact pins are the regression net (still ==, no epsilon). Empty stamp span routes to the unstamped path bit-identically, so the stamped normal overload's unstamped wrapper keeps 7.2's normal pins exact.


## 7.4 — swept-collision primitives (swept_contact, 49e48db)
- **Slab-clip TOI** in the box-local frame (rotate p0 and relative velocity by `conjugate(orient)` — exact, no sqrt): per local axis clip the interval `[0, dt]` to `|x0 + v·t| ≤ h`; hit = last entry, valid iff ≤ first exit. Entry axis recorded when `ta > t0` strictly ⇒ ties (diagonal corner approaches) resolve to the LOWER axis index — deterministic tie order, don't pin normals on ties.
- **No tunneling is structural, not tuned**: the hit is the interval entry, so pass-through at any speed is impossible. Pinned across speed decades 1..1e12 with bit-exact `t == (-hi - x0)/v` (identity orient + axis-aligned ⇒ `rotate(identity, v)` is bit-exact: `u=0` kills every cross term; FMA can't perturb it).
- **Start-inside semantics**: no entry plane crossed ⇒ hit at t=0 with the **earliest-exit-plane normal negated** (push-out side). Trap fixed during dev: exit axis must be tracked against uncapped `min tb`, NOT against the tick-capped window — exit beyond dt still needs a normal. Inside + zero relative velocity ⇒ no sweep event (nullopt); the later penalty contacts (7.8) handle resting overlap.
- **Capsule = L∞ inflation** (`h += radius`): conservative — hits EARLY by ≤ `r(√2−1)` of path at edges/corners, NEVER late (both bounds pinned; corner lead measured exactly). Fine for penalty contacts (they squash the excess); a precise GJK-style variant can layer later if arcade contact ever needs tighter edges.
- **Moving targets** (`target_vel` param): everything is relative velocity; the reported `pos` is the MOVER's world position at TOI (`p0 + vel·t`, not relative). Target outrunning the pursuer closes the slab window before t=0 ⇒ nullopt.
- Degenerate inputs (NaN/inf, half-extent < 0, negative/NaN dt, non-unit orient via `approximately_equal(length2,1,1e-6)` — rotate doesn't renormalize) ⇒ nullopt, never a throw, never a NaN hit. Mutation check: inverting the `t0 > dt` guard fails the suite (proves the pins bite, green-on-first-run wasn't vacuous).
- API is pose-free like terrain: callers sample both endpoints at ticks and pass `orientation()` for the box attitude (7.1); pure function of args, bit-identical repeats pinned.

## 7.5 — registry extension (game_attr_keys + cook fixtures, abd308f)
- Attr registry is a pure membership set: `kPhase5_attr_keys` keeps its Phase-5 name (marks origin) and grows 15→18; `hulldef.cpp` attr gate is generic so the new keys flow through cook with zero extra wiring. Consumers land later: 7.6 (drag), 7.7 (heat), 7.10 (clamp strength).
- `dock.port` acceptance = a **new G10 hullvalidate gate**, not a type allow-list (hull cooklet is data-driven — any `type` string parses). G10 (utility mount + positive `dock.clamp_strength`/`mass` + `port.undock` facet) is keyed on `dock.` prefix and appended AFTER G9 so every existing gate-error-order pin stays byte-identical. Dark-not-dead is deliberately NOT cooked — destroyed-port refusal is runtime (7.10); cook only refuses physically-unholdable authoring.
- **Trap (real bug, caught by fixture decode)**: `comp.attrs.emplace_back(r.str(), r.f64())` — argument evaluation order in a single call is *unspecified*; GCC consumed the f64 before the str and corrupted the Reader stream ("DEF0 payload truncated/malformed" on a fixture that encoded fine). Any stateful-Reader read must be separate sequenced statements.
- Gate-order pins (G1 fails before G2 etc.) + `run_pass` full-hull pattern make new gates cheap: mk_hull gained `hp_dockport` + `dockport_1` carrying the full contract, so every existing PASS test now also proves G10 doesn't over-fire; 7 G10 blocks cover each clause + a `coupling.kin` non-dock pass-through.
- Cook-fixture policy honored: dock parse cases are inline TOML strings (zero golden drift); static `test_ship.def.toml` untouched, both `.golden` files byte-identical.
- Registry typo case: `dock.clamp_strengths` (plural) must fail `registry` — membership is exact-match, no prefix leniency.

## 7.6 — arcade atmosphere/aero (sim_aero, 208cfb)
- Model is hand-computable end-to-end: `rho = rho0*exp(-alt/H)`, `q = ½ρ|v_rel|²`, `F = q*A*(CL*u_hat - CD*v_hat)`; atmosphere lives on `body_def` as `atm_rho0_kg_m3` / `atm_scale_height_m` (hand-authored SI this phase, cooked world schema stays Phase 8 per #45d).
- `add_body` gates: finite atm fields, `rho0 >= 0`; `rho0 > 0` additionally requires `H > 0` and `well_radius > 0` — an airless moon simply authors `rho0 = 0`.
- `#17` zero-force gates live inside `aero_forces` (no-atmo / no-well / outside-well => zero structurally) — both the outside-well gate and the CL dead-zone (`aoa >= 2*a_max`) were mutation-killed.
- Forgiving stall: `CL(a) = lift_coeff * sin(pi*a/(2*a_max))` for `a < 2*a_max`, exactly 0 past — peaks at exactly `lift_coeff` at authored `a_max`, never negative, no cliff. `a_max < pi/2` is a hard param gate so the dead zone is guaranteed to exist.
- Spin co-air: `air_vel = omega x (pos - center)` rides `v_rel` — pinned with the moon spinning at 1 rad/s.
- Terminal-velocity gotcha (real physics, not a bug): with the 7.14-scale fixture (H=1e9, 24 km fall) the Euler attractor is the LOCAL `vt_eff(y)` — the body sinks into denser air and trails the surface vt by ~1.6e-3. Test uses near-uniform H=1e12 for the attractor proof (residual 1.6e-6 = the H=1e12 vt_eff lag, port-verified); the density *curve* is pinned by direct `atmosphere_density` calls at H=1000 instead.
- fp64 sink at the attractor jitters +-ulp (upward jumps ~40% of ticks near vt): speed-monotonicity assertion carries a 1e-9 dip allowance (measured max dip 1.6e-10); the hard assertions are never-overshoot and final convergence.
- Glide trim: `tan(gamma) = CD/CL`, `v_g = sqrt(2mg*cos(gamma)/(rho*A*CL))`; attitude construction must tilt body -Z INTO the flow direction — a rotation about Z does nothing while fwd is -Z (first test version pinned aoa = pi/2 by exactly that bug). Phugoid tail after 10 s settles ~1e-5 (speed) / ~1e-7 (descent angle).
- Test-harness traps hit: doctest rejects `CHECK(a && b)` ("Expression Too Complex") — one binary comparison per CHECK; `math::kPi_d` (there is no `kPi`); TEST_CASE takes one arg in this doctest config; include is `<doctest/doctest.h>`.

## 7.7 — reentry heat number (aero_heat, bdb856f)
- Heat flux rides the 7.6 aero_result: `flux = absorb * dyn_q * |v_rel|` (= ½·absorb·ρ·v³, classic convective scaling) — no second sampling pass, one aero eval per tick feeds force AND heat.
- Budget is deliberately COOLING-FREE: heat only accrues; survivability IS the entry profile (v³ term: steep entries bake, shallow barely register) and loitering in dense air still cooks (honest budget behavior, hand-checkable asymptote: total heat ∝ absorbed drag work ≈ (absorb/CD)·ΔKE_diss).
- Tolerance lives OUTSIDE core state — it's read from the stats pipeline EVERY tick, so an authored per-hull base plus any modifier (7.5's `heat.tolerance`) flips outcomes with zero sim changes; the v0=2000 run proves it: identical bit-stream, tol 400 crosses at 6096, tol 400−150 crosses at 5618.
- Damage-as-data (6.x pattern): one apply_effect per over-tick, unique source id (`100+tick`) — apply_effect REJECTS a taken source id (invalid), unlike ms.add_modifier which needs add_source first; bundle stack ids unique per tick so nothing collides.
- Poison defense is two-layer and one layer is redundant by design: NaN/inf flux is caught by BOTH the flux gate and the `next`-finite gate (that mutant is EQUIVALENT, survives — expected); the overflow guard and negative-absorb gate are independently mutation-killed.
- Loop order is the pin contract: flux/heat from CURRENT state → over-check + damage → integrate. Python port of that exact order matched landing/cross ticks, over-tick counts and every fp32 integrity float bit-for-bit on the first C++ run.
- Trap: an overflow test needs `heat + flux*dt > DBL_MAX` — `1e308 + 1e308/60` is NOT an overflow (1.017e308 is finite); used 1.79e308 + 1.7e308·dt.

## 7.8 — landed-body contacts (#47a penalty, 0a29ae3)
- Pure per-call: `contact_vs_surface(surface_world, hull_vel, point_state)` ->
  `contact_hit{depth_m, normal_world, rel_vel}`; `penalty_force(hit, params)` ->
  normal spring-damper, clamped never-pull, hard `k*dmax` compliance ceiling.
  No stored contacts, no solver, no impulses (#47a). Poison gates (non-finite
  state/params, bad orientation, mu/c_t < 0) return nullopt/zero, never throw.
- **Deviation: the tangential friction channel is deliberately absent at 7.8.**
  The #29b sampler normal is radial everywhere — even on an authored pad — so
  the tangent plane is curved: any sustained tangential cap (Coulomb or
  tangential damper) drags a landed hull *along the curvature* and it never
  comes to rest (measured: pad-tangent 705 m/s sweep after one minute).
  "Stable rest on terrain" (checklist 7.8) actually *requires* no tangential
  channel with a radial-normal sphere model. Per #29 the capture zone's
  hold-down (#29g) + snap-to-rest (7.9) carry landed bodies; real Coulomb
  friction returns at 7.13 (on-foot) where the contact frame is a flat patch.
  `friction_mu`/`c_tangent_N_s_m` stay in `contact_params` and stay gate-live
  so 7.13 needs no API change.
- **Exact discrete fixed point**: with the integrator order (contact accel +
  gravity before `integrate_step`, velocity damping per tick), the resting
  penetration of n feet is `d = mg / (n*k + c/60)` — measured 0.0125000002
  (4 feet) and 0.050208333409 (1 foot) match it to the last digit. The naive
  hand pin `mg/nk` is the c->0 limit; pin the closed form, not a port output.
- Free-fall: first sampled contact tick for a drop from height h is
  `ceil(60*sqrt(2h/g))` (fall time is sampler-exact while airborne); overshoot
  peak of a zeta=1 drop pins just under the analytic `v*sqrt(m/k)` bound
  (0.08323 for the 15 m drop, ceiling `dmax=0.075` + spring-dominant tail).
- The surface snapshot (`surface_world`) carries terrain + authored stamps —
  stamps are NOT on `body_def` (7.3 sampler takes them per-call); tests stamp
  the snapshot. Terrain needs nonzero `relief_m` or the flatten level pins the
  *whole* surface (zero relief + level = global plane).
- API gotchas: `math::dquat{}` IS the identity (`{x,y,z,w=1}`, w defaults 1);
  `dquat{1,0,0,0}` is a 180° spin — state_ok rejects it as the earlier
  all-zero fear was wrong, but it is *not* identity. Probing exactly at the
  surface gives depth 0 -> nullopt (not touching); probe 1 m under for a
  depth==1 exact pad test. doctest cannot vector-compare: check
  `normal_world.x/y/z` components. Explicit-spring damping stability:
  keep `4*c*dt/m < 2` (zeta=1 at contact frequency, omega=sqrt(k/m)=10/s).
- Mutation-killed gates: normal-force sign, depth>0 no-contact gate, damper
  sign (energy-pump). Determinism: run==run memcmp over the whole 600-tick
  trajectory stream; an ulp nudge to `vel.x` diverges it.

## 7.10 — docking clamp (1384138 + fc79047)

- Docking reuses 7.9's capture machinery verbatim: `capture_engage` is
  now `template <Zone, World>` with explicit instantiations for
  `pad_def/pad_world` and `dock_port_def/port_world` — the port is just
  a capture zone welded to a host body (`port_at(port, host_rigid)`:
  pos/normal/vel ride the host hull pose, #24a rail-pose-is-rigid).
- Docked ≠ landed, one axis apart: `snap_to_rest` mates hull-local -Y
  (landers stand on their bellies); `snap_to_dock` mates hull-local +Z
  (the gear/airlock axis). A test with an on-axis gear offset or a
  junk roll ABOUT the keel is degenerate for both gates — the
  alignment-assist mutant survived exactly such fixtures. The
  load-bearing attitude gate is the pitched-junk fixture + keel-mate
  pin (`rotate(out.att, +Z) == port.normal`), because snap_to_dock
  back-computes `pos = target - R(att)·gear` and seats the gear
  TAUTOLOGICALLY at any attitude — gear-on-target CHECKs cannot see
  attitude. Off-axis gear (0,-2,0) also pins release torque
  (r×J = -2ŷ×20kN·ẑ -> dω_body.x = -4 rad/s, I=1e4).
- BRACE BUG FOUND (fc79047): dock.cpp's local `minimal_rotation` built
  its aggregate scalar-FIRST (`dquat{w, x, y, z}`) while `quat` is
  `{x, y, z, w}` — every non-degenerate assist was garbage; the
  identity/degenerate branches hid it. Symptom that cracked it: a
  composition-order flip that changed the probe output but NOT the
  test result (garbage composed with garbage still seats the gear).
  Composition is then `assist * ship` (engine operator* is Hamilton
  "a after b"; the assist is authored in world space, so it applies
  AFTER attitude). Grep lesson: when a probe contradicts a green
  suite, suspect the helper's field order before the convention.
- math::rotate EXONERATED: the "rotate returns (2,0,-1) for 180Y"
  claim was this same brace bug in a probe's own minimal_rotation
  wearing a disguise. Re-verified exact vs `to_mat4` for generic
  quats (180Y·Z and 50deg pitch·Z bit-agree). No core workaround
  needed; the probe `rotate_exact` mat4 detour was deleted.
- Engage tick pinned 5999, not the analytic shell crossing 6000:
  `capture_engage` reports the earliest entry into the tick window.
- Clamp is a hard bind, per decision: `docked_pose(host, clamp_att,
  clamp_pos)` is an exact pose projection every tick (pos = host pose
  + rot(clamp_att, port_local) - gear_local... algebraic, no spring,
  no solver); released push-off impulse is the departure path
  (`release_impulse` gives vel + J/m and torque domega about the
  gear offset). Destroyed clamp (`clamp_strength_N == 0`) never
  engages — dark-not-dead.
- Mutation gates that must stay killed: docked_pose gear term
  (1 CHECK), snap_to_dock aim-assist drop (4 CHECKs in the pitched
  fixture), capture safe_speed (shared with 7.9 pad suite — killing
  it burns 601 pad assertions too, expected).

## 7.9 — capture zones + snap-to-rest (f500921)

- Shape: `pad_def` (authored: body, local_dir on the surface, capture_radius,
  safe_speed, rest_depth) + `pad_at(rail_world, pad_def, tick) -> pad_world`
  (pose snapshot that rides spin: pos = center + R(t)·local_dir·R_body, vel =
  v_body + ω×r, normal = R(t)·local_dir). `capture_engage` decides from the
  tick snapshot only — pure, no stored zone state, so replay hashes ride it.
- #41 pinned hard: the safe-speed gate compares PAD-RELATIVE velocity.
  A world-stationary hull hovering 20 m over a rim pad on a spinning moon
  (rim ~1000 m/s) must NOT engage; a pad-matched hull must. A pole pad sits
  on the spin axis (zero rim velocity) and teaches nothing — fixture pads
  for spin tests go on the equator.
- #29e: too-fast/too-close flybys simply never engage; nothing "fails".
  The whole engage path is one `std::optional` — no failure states.
- Sweep = closed-form closest approach of the two LINEAR paths over the tick
  (tau clamped to [0,dt]), not the 7.4 OBB slab clip: the capture zone is a
  sphere with no solid surface to clip. The swept_hit shape is reused
  (pos/normal/t at the closest-approach instant); normal points pad->hull.
  Engagement sampled entirely from the pinned tick (#45e): checking the late
  snapshot against a start-of-tick trajectory is CHECK-false once the pad has
  spun past (equator fixture: ~67 deg in 100 s).
- `snap_to_rest` is algebraic, not a solver: pos = pad.pos - normal*rest_depth,
  vel = pad.vel exactly, attitude = minimal rotation taking hull-local -Y onto
  the pad normal, composed onto the current attitude. The 180-degenerate
  branch pins cross(x̂, from) as the axis (identity hull over +Y normal ->
  flip about -Z; w = cos(pi/2) ~ 1e-17 fp noise, x/y/z lockable).
- Determinism pin gotcha: a scripted descent re-pins velocity every tick, so
  a nudge must be applied at t==0 (position offset) to count. Engagement is
  a shell crossing: 1e-3 m/s shifts the crossing ~sub-tick (~4 ticks band),
  1e-9 lands on the SAME tick. Descent profile is authored (engines cancel
  well accel exactly, hold -1.5 m/s) — 200 m closure pins tick band
  [7900, 8060], measured 8001.
- API traps: `math::rotate(q, v)` for transforms; `math::quat_from_axis_angle`;
  `rail_world::add_body` returns optional-like — REQUIRE(has_value); doctest
  `-ts=pad_capture` for the suite (the -tc name is capture_zone); Approx on
  bare 0.0 is ABSOLUTE tolerance, but exact-zero pins (rest vel == 0.0) are
  safer than epsilon-on-zero here.
- Deviation: "release on player input" is a seat/input verb (7.11+), not this
  sim task; capture_engage stays stateless so the caller's input verb is the
  release. Mutation evidence: speed-gate-off mutant killed (601 failed
  assertions); always-engage at tick 0 killed by the analytic tick-band pin.
- 32,655 targeted assertions (the flyby sweep over 800 approach points is the
  bulk). CI 129 cases, 316 s.

## 7.11 — docked service gates + walk junction (88eb310)

- **No new state, no menu mode.** Everything derives from the 7.10
  `clamped_pair` + loadout tags, as pure functions — nothing stored,
  replay hashes ride it unchanged. `station_service_check(pair, station,
  facet, dark_tag)` and `mated_junction(...)` are referentially transparent.
- **dark-not-dead carries payload.** A gate that only answers
  yes/no can't drive a prompt (#28b). `service_check` returns
  `{available, reason, counter_id}`; the load-bearing dark test keeps
  the counter's service facets, adds the dark tag, and pins
  `reason == "counter dark"` AND `counter_id == "counter_truck"` —
  a fixture that only checked availability let the dark mutant survive.
- **Deterministic scan = loadout mount order.** First facet-matching
  counter wins; no unordered containers touched.
- **Walk junction is derived, never stored.** Edge exists iff: pair
  clamped + ship has airlock facet + station port names a section beside
  the clamp. Open iff both endpoint sections hold pressure. Fail-closed
  on a missing section report (`if (it == secs.end()) return false;`).
- **Round-trip pin gotchas (fixture truths learned the hard way):**
  - A hull starting ON the capture shell (z=130 with radius 30) engages
    at tick 0 — start outside; swept entry then lands deterministically
    (2004 here, one tick before the analytic shell crossing, like 7.10's
    5999-vs-6000).
  - `docked_pose` is pure — called after release it projects the
    DEPARTED ship. The release-instant pose is the last clamped pose,
    captured inside the loop before `release_impulse`.
  - Gear local +Z mates the port normal, so r x J = 0 at release:
    departure is pure translation (no spin to perturb the flight pin).
  - Flight pin must mirror the per-tick fp64 sum (`+= 1.0*dt` N times is
    NOT bit-identical to `N*dt` — ~1e-12 drift). Closed-form pins fail.
  - `release_impulse` mass-scaling is a real gate: 1000 N*s / 1000 kg
    = 1 m/s; a dropped `params` argument (default mass 1.0) launches at
    1000 m/s.
- **Mutants killed:** snap skipped (engage without clamp), engage
  disabled, release one tick late (flips count/timing + flight pins).
- **Deviation:** test suite named `docked_walk` (not `docked_walk_gates`
  as the checklist predicted); done-when (b) round trip lives in it as
  `docked_walk_round_trip`.
