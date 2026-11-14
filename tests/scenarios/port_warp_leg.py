#!/usr/bin/env python3
"""8.14 warp-leg scenario — the fp64 flight-recorder port (hand-calc pins).

Mirrors, op-for-op (same IEEE grouping), what the C++ engine does every
canonical tick for the 8.14 scripted scenario:
  rail_world positions (root + circular child rail, core/src/rails.cpp)
  well_g_magnitude / well_accel_at / total_accel dominance blend
      (core/src/gravity.cpp, core/src/well_blend.cpp; dominance_band 0.25)
  integrate_step_warp_dt linear half (vel += a*dt; pos += (vel*mult)*dt)
  replay.cpp safe-bubble wall hook (gate mult > 1; disp = (vel*mult)*dt)
  bubble_warp_contact quarter-coeff roots + 2^-40 surface band
      (core/src/safe_bubble.cpp; pinned quadratic order, do not simplify)
  in_safe_bubble (dot(rel,rel) - r*r < 0, strict; d == r is surface)
  ftl.cpp machine (engage/step/disengage ramps, anchor arithmetic)
  Fnv1a64 (core/include/ve/core/fnv.h) over state_hash(tick, state)
  reservoir value += (supply - load - leak) * dt with load = ftl.flow

Run: python3 port_warp_leg.py  -> prints the pin block the C++ test asserts.
Python floats are IEEE doubles; math.sin/cos ride the same glibc libm the
C++ pins were derived from (same technique as the 8.11 rail pins).
"""
import math
import struct

M64 = (1 << 64) - 1
TAU = 6.28318530717958647692528676655900577  # kTau_d (rounds to the 2*pi double)
DT = 1.0 / 60.0
BAND = 0.25

def tick_to_seconds(t): return t / 60.0

class Body:
    def __init__(self, surface, well, taper, g, bubble, safe_speed,
                 parent=-1, orbit_r=0.0, period=0.0, phase=0.0):
        assert surface > 0
        assert well == 0.0 or well > surface
        assert bubble == 0.0 or bubble >= max(surface, well)
        assert bubble == 0.0 or safe_speed > 0.0
        self.surface, self.well, self.taper, self.g = surface, well, taper, g
        self.bubble, self.safe_speed = bubble, safe_speed
        self.parent, self.orbit_r, self.period, self.phase = parent, orbit_r, period, phase

def rail_offset(b, t):
    if b.parent == -1: return (0.0, 0.0, 0.0)
    theta = b.phase + TAU * (tick_to_seconds(t) / b.period)
    return (b.orbit_r * math.cos(theta), 0.0, -b.orbit_r * math.sin(theta))

def position(bodies, i, t):
    chain = []
    j = i
    while bodies[j].parent != -1:
        chain.append(j); j = bodies[j].parent
    chain.append(j)
    x = y = z = 0.0
    for k in reversed(chain):
        dx, dy, dz = rail_offset(bodies[k], t)
        x, y, z = x + dx, y + dy, z + dz
    return (x, y, z)

def well_g(b, r):
    if b.well <= 0.0 or r >= b.well: return 0.0
    if r <= b.taper: return b.g
    u = (r - b.taper) / (b.well - b.taper)
    omu = 1.0 - u
    return b.g * omu * omu * (1.0 + 2.0 * u)

def dot(u, v): return (u[0]*v[0] + u[1]*v[1]) + u[2]*v[2]
def length(v): return math.sqrt(dot(v, v))

def well_accel_at(b, center, pos):
    tc = (center[0]-pos[0], center[1]-pos[1], center[2]-pos[2])
    r = length(tc)
    if r == 0.0: return (0.0, 0.0, 0.0)
    g = well_g(b, r)
    if g == 0.0: return (0.0, 0.0, 0.0)
    k = g / r
    return (tc[0]*k, tc[1]*k, tc[2]*k)

def clamp01(v): return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)
def smoothstep01(u): return u * u * (3.0 - 2.0 * u)

def total_accel(bodies, pos, t):
    n = len(bodies)
    accs = [well_accel_at(b, position(bodies, i, t), pos) for i, b in enumerate(bodies)]
    gmax = 0.0
    for a in accs: gmax = max(gmax, length(a))
    if gmax == 0.0: return (0.0, 0.0, 0.0)
    def w(i):
        a = length(accs[i])
        if a == 0.0: return 0.0
        return smoothstep01(clamp01((a / gmax - (1.0 - BAND)) / BAND))
    wsum = 0.0
    for i in range(n): wsum += w(i)
    acc = [0.0, 0.0, 0.0]
    for i in range(n):
        wi = w(i)
        if wi == 0.0: continue
        k = wi / wsum
        acc[0] = acc[0] + accs[i][0] * k
        acc[1] = acc[1] + accs[i][1] * k
        acc[2] = acc[2] + accs[i][2] * k
    return tuple(acc)

def in_safe_bubble(bodies, pos, t):
    """core/src/safe_bubble.cpp in_safe_bubble: strict interior, dot-r^2."""
    for i, b in enumerate(bodies):
        r = b.bubble
        if not r > 0.0: continue
        c = position(bodies, i, t)
        rel = (pos[0]-c[0], pos[1]-c[1], pos[2]-c[2])
        if dot(rel, rel) - r * r < 0.0: return i
    return None

def bubble_warp_contact(bodies, p0, disp, t):
    """core/src/safe_bubble.cpp bubble_warp_contact, pinned quadratic order."""
    best = None  # (body, u, pos, inward)
    for i, b in enumerate(bodies):
        r = b.bubble
        if not r > 0.0: continue
        c = position(bodies, i, t)
        rel = (p0[0]-c[0], p0[1]-c[1], p0[2]-c[2])
        a = dot(disp, disp)
        if a == 0.0: continue
        bq = dot(rel, disp)
        cq = dot(rel, rel) - r * r
        e = (rel[0]+disp[0], rel[1]+disp[1], rel[2]+disp[2])
        fr = dot(e, e) - r * r
        band = r * r * 2.0**-40
        f0 = 0.0 if abs(cq) <= band else cq
        f1 = 0.0 if abs(fr) <= band else fr

        u = 0.0
        inward = False
        if f0 == 0.0:
            if f1 == 0.0: continue
            u = 0.0
            inward = f1 < 0.0
        elif f0 < 0.0 and f1 < 0.0:
            continue
        elif f0 < 0.0:
            disc = bq * bq - a * cq
            u2 = (-bq + math.sqrt(disc)) / a
            if not u2 > 0.0 or u2 > 1.0: continue
            u = u2
            inward = False
        else:
            disc = bq * bq - a * cq
            if disc <= 0.0: continue
            u1 = (-bq - math.sqrt(disc)) / a
            if not u1 > 0.0 or u1 > 1.0: continue
            u = u1
            inward = u < 1.0

        if u == 0.0:
            bpos = p0
        else:
            wv = (rel[0]+disp[0]*u, rel[1]+disp[1]*u, rel[2]+disp[2]*u)
            bpos = (c[0]+wv[0], c[1]+wv[1], c[2]+wv[2])
        if best is None or u < best[1]:
            best = (i, u, bpos, inward)
    return best

# FNV-1a 64 (fnv.h: MSB-first byte mixing)
FNV_OFF = 1469598103934665603
FNV_PRIME = 1099511628211
def mix_byte(h, b): return ((h ^ b) * FNV_PRIME) & M64
def mix_u64(h, v):
    for shift in range(56, -1, -8):
        h = mix_byte(h, (v >> shift) & 0xFF)
    return h
def bits(d): return struct.unpack('<Q', struct.pack('<d', d))[0]
def state_hash(tick, pos, vel, att, omega):
    h = FNV_OFF
    h = mix_u64(h, tick)
    for d in (pos[0], pos[1], pos[2], vel[0], vel[1], vel[2],
              att[0], att[1], att[2], att[3],
              omega[0], omega[1], omega[2]):
        h = mix_u64(h, bits(d))
    return h
def interval_hash(hashes):
    h = FNV_OFF
    for v in hashes: h = mix_u64(h, v)
    return h

# ---------------------------------------------------------------- fixture
# Thalassa: root, static. Ferrous: child rail at 1.52 AU, 100-year period.
AU = 1.495978707e11
thal = Body(6.371e6, 6.371e7, 3.1855e7, 9.81, 6.371e7, 2.5e5)
ferr = Body(3.3895e6, 4.0e7, 2.0e7, 3.71, 4.0e7, 2.5e5,
            parent=0, orbit_r=1.52*AU, period=3.15576e9, phase=0.0)
bodies = [thal, ferr]

SPEED_MULT = 5000.0   # ftl.speed_mult aggregate
SPOOL_S    = 10.0     # ftl.spool_s
FLOW       = 120.0    # ftl.flow (kg/s); reservoir drains FLOW*dt = 2.0/tick
TANK0      = 60000.0
ARRIVAL_R  = 3.89e7   # Ferrous orbit-insert radius, > surface_radius

IDLE, UP, WARP, DOWN = 0, 1, 2, 3
def drive_ok(mult, spool): return mult > 1.0 and spool > 0.0

def run(apply_door=True, blind_at=None, stop_at=None):
    pos = (6.373e6, 0.0, 0.0)
    vel = (2.0e5, 0.0, 0.0)
    att, omega = (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
    phase, mult, rate, anchor_t, anchor_m = IDLE, 1.0, 0.0, 0, 1.0
    tank = TANK0
    t_engage = crossing = arrival = t_wall = None
    prev_contact = False
    fires = 0
    hull = 1000.0
    hashes = []
    states = {}
    t = 0
    while True:
        # glue: engage door (first tick strictly outside the departure bubble)
        if ((apply_door and phase == IDLE and t_engage is None
                and in_safe_bubble(bodies, pos, t) is None)
                or (blind_at is not None and t == blind_at
                    and phase == IDLE and t_engage is None)):
            phase = UP
            mult = 1.0
            rate = (SPEED_MULT - 1.0) / SPOOL_S
            anchor_t, anchor_m = t, 1.0
            t_engage = t
        # ftl_step (ftl.cpp, exact)
        if phase != IDLE and not drive_ok(SPEED_MULT, SPOOL_S):
            phase, mult, anchor_t, anchor_m = IDLE, 1.0, t, 1.0
        elif phase == IDLE:
            mult = 1.0
        elif phase == UP:
            m = anchor_m + rate * tick_to_seconds(t - anchor_t)
            if m >= SPEED_MULT:
                mult, phase, anchor_t = SPEED_MULT, WARP, t
            else:
                mult = m
        elif phase == WARP:
            mult = SPEED_MULT
        elif phase == DOWN:
            m = anchor_m - rate * tick_to_seconds(t - anchor_t)
            if m <= 1.0:
                mult, phase = 1.0, IDLE
            else:
                mult = m
        cmd_mult = mult
        p0 = pos
        acc = total_accel(bodies, pos, t)
        vel = (vel[0] + acc[0]*DT, vel[1] + acc[1]*DT, vel[2] + acc[2]*DT)
        pos = (pos[0] + (vel[0]*cmd_mult)*DT,
               pos[1] + (vel[1]*cmd_mult)*DT,
               pos[2] + (vel[2]*cmd_mult)*DT)
        contact = None
        if cmd_mult > 1.0:
            disp = ((vel[0]*cmd_mult)*DT, (vel[1]*cmd_mult)*DT, (vel[2]*cmd_mult)*DT)
            contact = bubble_warp_contact(bodies, p0, disp, t)
            if contact is not None:
                pos = contact[2]
        if contact is not None and contact[0] == 0 and t_wall is None:
            t_wall = t
        if contact is not None and contact[0] == 1:
            if not prev_contact:
                fires += 1
                hull -= 250.0
                if phase != IDLE:
                    phase, anchor_t, anchor_m = DOWN, t, mult
                if crossing is None: crossing = t
        prev_contact = contact is not None
        if phase != IDLE:
            tank = tank + (0.0 - FLOW - 0.0) * DT
        hashes.append(state_hash(t, pos, vel, att, omega))
        states[t] = (pos, vel, mult)
        c1 = position(bodies, 1, t)
        d1 = math.sqrt((pos[0]-c1[0])**2 + (pos[1]-c1[1])**2 + (pos[2]-c1[2])**2)
        if d1 <= ARRIVAL_R:
            arrival = t
            break
        if stop_at is not None and t + 1 >= stop_at:
            break
        t += 1
        assert t < 60000, "scenario did not terminate"
    return dict(t_engage=t_engage, crossing=crossing, arrival=arrival,
                t_wall=t_wall, pos=pos, vel=vel, tank=tank, hull=hull,
                fires=fires, interval=interval_hash(hashes), states=states)

r = run()
T1, X, A = r['t_engage'], r['crossing'], r['arrival']
print(f"engage_tick   = {T1}")
print(f"crossing_tick = {X}")
print(f"arrival_tick  = {A}")
print(f"total_ticks   = {A + 1}")
print(f"burn_ticks    = {X + 600 - T1}")
print(f"final_tank    = {r['tank']!r}")
print(f"hull          = {r['hull']!r} fires = {r['fires']}")
print(f"mult[T1+600]  = {r['states'][T1+600][2]!r}")
print(f"mult[X+600]   = {r['states'][X+600][2]!r}")
print(f"arrival_pos   = {tuple(repr(v) for v in r['pos'])}")
print(f"interval_hash = {r['interval']:#018x}")
for k in (X, X+1, X+300, X+599, X+600):
    p = r['states'][k][0]
    c = position(bodies, 1, k)
    print(f"hold t={k}: pos=({p[0]!r},{p[1]!r},{p[2]!r}) dist_wall={math.dist(p,c)-4.0e7:+.6f}")

if __name__ == "__main__" and "--blind" in __import__("sys").argv:
    # counterfactual: engage DEEP inside the departure bubble, no door.
    B0 = 10000
    b = run(apply_door=False, blind_at=B0, stop_at=B0 + 1200)
    thal_c = (0.0, 0.0, 0.0)
    mo = max(abs(math.dist(b['states'][t][0], thal_c) - 6.371e7)
             for t in range(b['t_wall'], max(b['states']) + 1))
    mult_full = all(b['states'][t][2] == SPEED_MULT
                    for t in range(B0 + 601, B0 + 1200))
    print(f"blind engage={b['t_engage']} inside={in_safe_bubble(bodies, b['states'][B0-1][0], B0)} "
          f"wall={b['t_wall']} crossing={b['crossing']} mult_full={mult_full}")
    print(f"blind max_over_since_wall = {mo!r}")
