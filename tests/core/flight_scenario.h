#pragma once
// Shared scripted flight for Phase 7 done-when (c): the 7.14
// flight_landing_scenario simulation, extracted so the core harness
// (test_flight_landing_scenario.cpp) and the renderer proof
// (gfx/test_flight_visible.cpp) replay the SAME flight from one source.
// Behavior-identical to the pre-7.15 inline copy — pins/counts unchanged;
// the only additions are the per-tick pose track and the pad anchor, which
// the renderer consumes. See notes.md 7.14/7.15 for the design lessons.
//
// NOTE (renderer side): 7.8's contact model has NO sustained tangential
// force, so a captured stand on the SPINNING pad coasts slowly across the
// surface while the legs ride the curvature (post-capture poses drift).

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ve/core/math/quat.h"
#include "ve/core/math/vec.h"
#include "ve/core/sim/aero.h"
#include "ve/core/sim/capture.h"
#include "ve/core/sim/contacts.h"
#include "ve/core/sim/rails.h"
#include "ve/core/sim/replay.h"
#include "ve/core/sim/rigid.h"
#include "ve/core/sim/time.h"
#include "ve/core/sim/well_blend.h"

namespace ve_test::flight {

using namespace ve::core;
using namespace ve::core::sim;

// ---------------------------------------------------------------------------
// World: spinning planet with an atmosphere, one equatorial pad.
//   surface g = 3.0, pad rim speed = spin_rate * r = 1 m/s (non-zero: the
//   capture must be pad-relative, #41 — a world-frame tolerance would lie).
// ---------------------------------------------------------------------------
constexpr double kR = 2'000.0;
constexpr double kG = 3.0;
constexpr double kSpin = 0.0005;

inline rail_world make_world() {
    rail_world rw;
    body_def b;
    b.surface_radius = kR;
    b.surface_g = kG;
    b.well_radius = 100'000.0;
    b.well_taper_start = 60'000.0;
    b.atm_rho0_kg_m3 = 1.0;
    b.atm_scale_height_m = 800.0;
    b.spin_axis = {0.0, 1.0, 0.0};
    b.spin_rate_rad_s = kSpin;
    const auto added = rw.add_body(std::move(b));
    REQUIRE(added.has_value());
    return rw;
}

inline pad_def make_pad(std::int32_t body) {
    return pad_def{
        .body = body,
        .local_dir = {1.0, 0.0, 0.0},  // equator at t=0 -> rim speed 1 m/s
        .capture_radius_m = 100.0,
        .safe_speed_m_s = 6.0,
        // equilibrium penalty squat: m*g / (4 legs * k) = 6000 / 800'000
        .rest_depth_m = 0.0075,
    };
}

// Ship: 2000 kg, T/W = 5, four legs at the 7.8 gear-plane convention
// (origin IS the gear plane; snap_to_rest pins origin rest_depth under the
// pad). c_n per leg 10'000: zeta = c / (2*sqrt(k*m/4)) = 0.5; stability
// margin 4*c*dt/(m/4) = 1.33 < 2 (determinism policy reference).
constexpr double kMass = 2'000.0;
constexpr double kTmax = 30'000.0;
constexpr contact_params kLegs{
    .k_pushout_N_m = 200'000.0, .c_normal_N_s_m = 10'000.0,
    .friction_mu = 0.8, .c_tangent_N_s_m = 10'000.0};
constexpr std::array<math::dvec3, 4> kFeet{
    math::dvec3{0.8, 0.0, 0.6}, math::dvec3{0.8, 0.0, -0.6},
    math::dvec3{-0.8, 0.0, 0.6}, math::dvec3{-0.8, 0.0, -0.6}};

constexpr aero_params kAero{
    .ref_area_m2 = 15.0, .drag_coeff = 0.05,
    .lift_coeff = 0.8, .max_aoa_rad = 0.35};
// Cruise lift pin (used for the aoa schedule below): at 45 m/s / 300 m
// altitude, rho = exp(-300/800) = 0.687, q = 695, CL_req = 6000/(695*15)
// = 0.575 -> aoa = (2*a_max/pi)*asin(0.575/0.8) = 0.179 rad < a_max. OK.

// Timeline (ticks at 60/s):
//   rest          0..59        legs squat at snap rest depth
//   takeoff       60..2399     climb to ~350 m at 12 m/s over the column
//   cruise        2400..4199   accelerate to 45 m/s, hold 300 m (aoa live)
//   glide         4200..5999   ZERO thrust; stall excursion (aoa 0.45 >
//                              a_max) for the first 15 s, then aoa 0.30
//   approach      6000..       turn back onto the pad column, capture
constexpr sim::tick_t kT_LIFT = 60, kT_CRUISE = 2400, kT_GLIDE = 4200,
                      kT_APPR = 6'000, kT_END = 21'600;
constexpr double kV_CRUISE = 45.0;

inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline math::dvec3 clamp_vec(math::dvec3 v, double mag) {
    const double m = math::length(v);
    return (m > mag) ? v * (mag / m) : v;
}

// Rotation matrix (columns = target world axes of body X,Y,Z) -> dquat.
// Trace method, scalar-last aggregate (hand-rolled: Tier-1 test helper).
inline math::dquat quat_from_axes(math::dvec3 x, math::dvec3 y, math::dvec3 z) {
    const double m00 = x.x, m01 = y.x, m02 = z.x;
    const double m10 = x.y, m11 = y.y, m12 = z.y;
    const double m20 = x.z, m21 = y.z, m22 = z.z;
    const double tr = m00 + m11 + m22;
    double w, qx, qy, qz;
    if (tr > 0.0) {
        const double s4 = std::sqrt(tr + 1.0) * 2.0;
        w = 0.25 * s4; qx = (m21 - m12) / s4; qy = (m02 - m20) / s4; qz = (m10 - m01) / s4;
    } else if (m00 > m11 && m00 > m22) {
        const double s4 = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        w = (m21 - m12) / s4; qx = 0.25 * s4; qy = (m01 + m10) / s4; qz = (m02 + m20) / s4;
    } else if (m11 > m22) {
        const double s4 = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        w = (m02 - m20) / s4; qx = (m01 + m10) / s4; qy = 0.25 * s4; qz = (m12 + m21) / s4;
    } else {
        const double s4 = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        w = (m10 - m01) / s4; qx = (m02 + m20) / s4; qy = (m12 + m21) / s4; qz = 0.25 * s4;
    }
    return math::normalize(math::dquat{qx, qy, qz, w});
}

// ---------------------------------------------------------------------------
// Scenario run
// ---------------------------------------------------------------------------
struct run_result {
    std::vector<std::uint64_t> hashes;
    std::vector<sim::rigid_state> poses;  // poses[t] == state at END of tick t
    math::dvec3 pad_pos0{};               // pad world pos at t=0 (render anchor)
    sim::tick_t engage_tick = -1;
    double engage_rel_speed = 0.0, engage_world_speed = 0.0, pad_speed = 0.0;
    double rest_depth_30 = 0.0;      // leg squat during pad rest (t=30)
    double max_altitude = 0.0, max_dyn_q = 0.0, max_aoa = 0.0;
    sim::tick_t max_aoa_tick = -1;
    double worst_pos = 0.0, worst_dhi = 0.0, worst_dlo = 10.0, worst_v = 0.0;
    double cl_at_max_aoa = 0.0, cl_max = 0.0;
    double speed_glide_start = 0.0, speed_glide_end = 0.0;
    double alt_glide_start = 0.0, alt_glide_end = 0.0;
    double max_omega = 0.0;
    bool ok = true;  // every tick state_ok + finite pins
};

inline run_result run_scenario() {
    run_result R;
    const rail_world rw = make_world();
    const std::int32_t planet = 0;
    const pad_def pad = make_pad(planet);
    const rigid_params rp{.mass = kMass, .inertia_diag = {2e5, 2e5, 2e5}};

    const auto pw0 = pad_at(rw, pad, 0);
    REQUIRE(pw0.has_value());
    R.pad_pos0 = pw0->pos;
    // Start AT REST on the pad via the shipped snap (so tick 0 is the
    // equilibrium squat pose, not a hand-tuned approximation).
    rigid_state s = snap_to_rest({.pos = pw0->pos, .vel = pw0->vel}, *pw0, pad);

    bool landed = false;
    // Capture arms only once the ship has actually left the pad (the
    // at-rest start sits INSIDE the capture zone; probing there would
    // "engage" at tick 0).
    bool capture_armed = false;
    double alt_at_appr = 0.0;
    for (sim::tick_t t = 0; t < kT_END; ++t) {
        const auto pw = pad_at(rw, pad, t);
        REQUIRE(pw.has_value());
        const math::dvec3 center = *rw.position(planet, t);
        const math::dvec3 n_hat = math::normalize(s.pos - center);
        const math::dvec3 g_vec = total_accel(rw, s.pos, t);

        if (!capture_armed && math::length(s.pos - center) - kR > 150.0)
            capture_armed = true;

        // --- capture probe (7.9 order: probe, then step with results) ---
        if (!landed && capture_armed) {
            if (const auto hit = capture_engage(pad, *pw, s.pos, s.vel, t); hit) {
                R.engage_tick = t;
                const math::dvec3 rel = s.vel - pw->vel;
                R.engage_rel_speed = math::length(rel);
                R.engage_world_speed = math::length(s.vel);
                R.pad_speed = math::length(pw->vel);
                s = snap_to_rest(s, *pw, pad);
                landed = true;
            }
        }

        // --- scripted pilot: command stream = f(state, tick), replay shape ---
        tick_command cmd{};
        if (!landed) {
            const math::dvec3 q = s.pos - pw->pos;              // pad-relative
            const double alt = math::length(s.pos - center) - kR;
            if (t == kT_APPR) alt_at_appr = alt;
            const math::dvec3 v_rel_pad = s.vel - pw->vel;
            const math::dvec3 tangent = math::normalize(math::cross({0.0, 1.0, 0.0}, n_hat));
            const double v_n = math::dot(s.vel - pw->vel, n_hat);
            const math::dvec3 p_lat = q - n_hat * math::dot(q, n_hat);
            const math::dvec3 v_lat = v_rel_pad - n_hat * math::dot(v_rel_pad, n_hat);

            math::dvec3 a_cmd{};
            double thrust_scale = 1.0;
            double aoa_des = 0.0;
            if (t < kT_LIFT) {
                a_cmd = {}; thrust_scale = 0.0; aoa_des = 0.0;  // legs hold us
            } else if (t < kT_CRUISE) {
                // Rate-shaped climb: v_des shrinks near the ceiling so the
                // ship eases onto 350 m instead of spring-overshooting it.
                const double v_des = std::min(12.0, 0.06 * (350.0 - alt));
                const double a_n = clampd(1.5 * (v_des - v_n), -3.0, 4.0);
                const math::dvec3 a_l =
                    clamp_vec(-0.02 * p_lat - 0.30 * v_lat, 2.0);
                a_cmd = n_hat * a_n + a_l;
                aoa_des = 0.05;
            } else if (t < kT_GLIDE) {
                const double v_t = math::dot(s.vel - pw->vel, tangent);
                const double a_t = clampd(0.2 * (kV_CRUISE - v_t), -1.6, 1.6);
                const double a_n = clampd(0.6 * (300.0 - alt) + 0.8 * (0.0 - v_n), -3.0, 4.0);
                a_cmd = tangent * a_t + n_hat * a_n;
                aoa_des = -1.0;  // closed-loop lift inversion below
            } else if (t < kT_APPR) {
                thrust_scale = 0.0;  // GLIDE: zero main thrust, open-loop aoa
                aoa_des = (t < kT_GLIDE + 900) ? 0.45 : 0.30;  // stall excursion
            } else {
                // Approach: steer the pad column, descend at 3 m/s through
                // the capture zone (below safe_speed 6).
                const double h_des = clampd(alt_at_appr - 3.0 * double(t - kT_APPR) * kSim_tick_dt,
                                            8.0, 300.0);
                const math::dvec3 target = pw->pos + pw->normal * h_des - s.pos;
                a_cmd = clamp_vec(0.004 * target - 0.16 * v_rel_pad, 2.5);
                aoa_des = -1.0;
            }

            // Closed-loop aoa from the lift equation (cruise/approach only).
            if (aoa_des < 0.0) {
                const double rho = atmosphere_density(1.0, 800.0, alt);
                const math::dvec3 air_v = coair_velocity(kSpin, {0.0, 1.0, 0.0}, center, s.pos);
                const double v_rel = math::length(s.vel - air_v);
                const double q_dyn = 0.5 * rho * v_rel * v_rel;
                const double cl_req = clampd(kMass * kG / (q_dyn * kAero.ref_area_m2), 0.0, 0.79);
                aoa_des = (2.0 * kAero.max_aoa_rad / math::kPi_d) * std::asin(cl_req / kAero.lift_coeff);
                aoa_des = clampd(aoa_des, 0.02, 0.32);
            }

            // Thrust demand -> hull -Y along thrust.
            const math::dvec3 f_req = kMass * (a_cmd - g_vec);
            const double t_mag = clampd(thrust_scale * math::length(f_req), 0.0, kTmax);
            cmd.thrust_body = {0.0, -t_mag, 0.0};
            math::dvec3 y_w = (t_mag > 1.0) ? math::normalize(g_vec - a_cmd) : -n_hat;
            // (y_w = world image of body +Y = -F_hat)

            // Forward (body -Z) = flow direction pitched up by aoa_des.
            const math::dvec3 air_v = coair_velocity(kSpin, {0.0, 1.0, 0.0}, center, s.pos);
            const math::dvec3 v_rel_air = s.vel - air_v;
            const double spd = math::length(v_rel_air);
            math::dvec3 fwd = tangent;  // low-speed fallback: fly downrange
            if (spd > 5.0) {
                const math::dvec3 v_hat = v_rel_air / spd;
                fwd = math::normalize(v_hat * std::cos(aoa_des) + n_hat * std::sin(aoa_des));
            }
            math::dvec3 z_raw = -fwd;
            math::dvec3 z_w = z_raw - y_w * math::dot(z_raw, y_w);
            if (math::length(z_w) < 1e-6) z_w = -tangent;
            z_w = math::normalize(z_w);
            const math::dvec3 x_w = math::cross(y_w, z_w);
            const math::dquat att_target = quat_from_axes(x_w, y_w, z_w);

            // Attitude PD in body frame (RCS; arcade authority, clamped).
            math::dquat qe = math::normalize(math::conjugate(s.att) * att_target);
            if (qe.w < 0.0) qe = qe * -1.0;
            const math::dvec3 e{qe.x, qe.y, qe.z};
            math::dvec3 tau = 2.0 * 1.3e5 * e - 2.9e5 * s.omega_body;
            const double tm = math::length(tau);
            if (tm > 4e5) tau = tau * (4e5 / tm);
            cmd.torque_body = tau;
        }

        // --- shared spine: gravity + command thrust/torque (4.7 path) ---
        math::dvec3 accel{}, torque{};
        assemble_tick_inputs(rw, cmd, t, rp, s, accel, torque);

        // --- aero (7.6): ONLY inside the well AND authored atmosphere ---
        const math::dvec3 air_v = coair_velocity(kSpin, {0.0, 1.0, 0.0}, center, s.pos);
        const aero_result ar = aero_forces(*rw.body(planet), center, air_v, s, kAero);
        accel = accel + ar.force * (1.0 / kMass);
        if (ar.speed_rel > 30.0) {
            R.max_dyn_q = std::max(R.max_dyn_q, ar.dyn_q);
            if (ar.aoa_rad > R.max_aoa) { R.max_aoa = ar.aoa_rad; R.max_aoa_tick = t; R.cl_at_max_aoa = ar.cl_eff; }
            R.cl_max = std::max(R.cl_max, ar.cl_eff);
        }

        // --- contacts (7.8): penalty legs against the spinning surface ---
        double tick_max_depth = -1.0;
        surface_world surf;
        surf.body = rw.body(planet);
        surf.center = center;
        surf.center_vel = *rw.velocity(planet, t);
        surf.orient = *rw.orientation(planet, t);
        surf.spin_omega = {0.0, kSpin, 0.0};
        for (const auto& foot : kFeet) {
            if (const auto hit = contact_vs_surface(surf, foot, s); hit) {
                tick_max_depth = std::max(tick_max_depth, hit->depth_m);
                accel = accel + penalty_force(*hit, kLegs) * (1.0 / kMass);
                if (t == 30) R.rest_depth_30 = std::max(R.rest_depth_30, hit->depth_m);
            }
        }
        if (landed && tick_max_depth >= 0.0) {
            // Band on the DEEPEST leg: on the spin pad a coasting stand
            // unloads single legs by design (7.8 "tangential motion coasts");
            // the load always rides on at least one leg inside the band.
            R.worst_dlo = std::min(R.worst_dlo, tick_max_depth);
            R.worst_dhi = std::max(R.worst_dhi, tick_max_depth);
        }

        if (t == kT_GLIDE) {
            R.speed_glide_start = math::length(s.vel - pw->vel);
            R.alt_glide_start = math::length(s.pos - center) - kR;
        }

        integrate_step(s, rp, accel, torque);
        R.ok = R.ok && state_ok(s);
        R.max_altitude = std::max(R.max_altitude, math::length(s.pos - center) - kR);
        R.max_omega = std::max(R.max_omega, math::length(s.omega_body));
        if (t == kT_GLIDE + 1'799) {
            R.speed_glide_end = math::length(s.vel - pw->vel);
            R.alt_glide_end = math::length(s.pos - center) - kR;
        }
        if (landed) {
            const double rel_pos = math::length(s.pos - pw->pos);
            R.worst_pos = std::max(R.worst_pos, rel_pos);
            R.worst_v = std::max(R.worst_v, math::length(s.vel - pw->vel));
        }
        R.poses.push_back(s);
        R.hashes.push_back(state_hash(t, s));
    }
    return R;
}

} // namespace ve_test::flight
