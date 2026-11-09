// Phase 5 task 5.5 — sim_thruster_forces: force-at-point machinery pins.
// Hand-computed r×F torque for an off-CoM engine; RCS pure couple (net
// force 0, moment exact); gimbal cone angle + clamp; world rotation of
// force under yaw-90 attitude; one real integrate_step tick from the
// outputs (engine off-CoM -> measurable omega growth); applied-force
// record order/skip rules (the Phase 6 drain input).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "ve/core/sim/rigid.h"
#include "ve/core/sim/thruster.h"

namespace {

using namespace ve::core;
using math::cross;
using math::dquat;
using math::dvec2;
using math::dvec3;
using math::quat_yaw;
using sim::apply_thrusters;
using sim::gimbal_deflected_axis;
using sim::integrate_step;
using sim::rigid_params;
using sim::rigid_state;
using sim::thruster_input;
using sim::thruster_static;

thruster_static make_thruster(dvec3 pos, double max_force, double gimbal_limit = 0.0,
                              dvec3 axis = dvec3(0, 0, -1)) {
    thruster_static t;
    t.position_local = pos;
    t.axis_local = axis;
    t.max_force = max_force;
    t.gimbal_limit_rad = gimbal_limit;
    return t;
}

bool approx3(const dvec3& a, const dvec3& b, double eps = 1e-9) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

}  // namespace

TEST_CASE("sim_thruster_forces") {
    // ---- 1. single engine off-CoM: world force + hand-computed torque ----
    // engine at (0,0,-4) pushing -Z, CoM at (0.5,0,0), identity attitude.
    // r = pos - com = (-0.5,0,-4); F = (0,0,-1000)
    // tau = r x F = (0*(-1000) - (-4)*0, (-4)*0 - (-0.5)*(-1000), 0) = (0,-500,0)
    std::vector<thruster_static> ts{make_thruster(dvec3(0, 0, -4), 1000.0)};
    std::vector<thruster_input> ins{thruster_input{1.0, dvec2(0, 0)}};
    auto r = apply_thrusters(dquat{}, dvec3(0.5, 0, 0), ts, ins);
    CHECK(approx3(r.force_world, dvec3(0, 0, -1000)));
    CHECK(approx3(r.torque_body, dvec3(0, -500, 0)));
    REQUIRE(r.applied.size() == 1);
    CHECK(r.applied[0].index == 0);
    CHECK(approx3(r.applied[0].force_local, dvec3(0, 0, -1000)));  // body == world (att identity)

    // throttle scales linearly; over-range clamps; zero throttle skips record.
    ins[0].throttle = 0.25;
    CHECK(approx3(apply_thrusters(dquat{}, {}, ts, ins).force_world, dvec3(0, 0, -250)));
    ins[0].throttle = 3.0;
    CHECK(approx3(apply_thrusters(dquat{}, {}, ts, ins).force_world, dvec3(0, 0, -1000)));
    ins[0].throttle = 0.0;
    auto off = apply_thrusters(dquat{}, {}, ts, ins);
    CHECK(off.applied.empty());
    CHECK(approx3(off.force_world, dvec3(0, 0, 0)));

    // ---- 2. RCS pure couple: net force 0, moment exact ----
    // pods at (+-3,0,0), forces +z and -z with F=100:
    // tau = (3,0,0)x(0,0,100) + (-3,0,0)x(0,0,-100) = (0,-300,0)+(0,-300,0) = (0,-600,0)
    std::vector<thruster_static> rcs{
        make_thruster(dvec3(3, 0, 0), 100.0, 0.0, dvec3(0, 0, 1)),
        make_thruster(dvec3(-3, 0, 0), 100.0, 0.0, dvec3(0, 0, -1)),
    };
    std::vector<thruster_input> rin(2, thruster_input{1.0, dvec2(0, 0)});
    auto rc = apply_thrusters(dquat{}, {}, rcs, rin);
    CHECK(approx3(rc.force_world, dvec3(0, 0, 0)));
    CHECK(approx3(rc.torque_body, dvec3(0, -600, 0)));
    CHECK(rc.applied.size() == 2);                    // order kept
    CHECK(rc.applied[0].index == 0);
    CHECK(rc.applied[1].index == 1);

    // strafe pair: same direction -> translation only, no torque about CoM.
    rcs[1].axis_local = dvec3(0, 0, 1);
    auto st = apply_thrusters(dquat{}, {}, rcs, rin);
    CHECK(approx3(st.force_world, dvec3(0, 0, 200)));
    CHECK(approx3(st.torque_body, dvec3(0, 0, 0)));

    // ---- 3. gimbal cone: exact angle, then clamp ----
    thruster_static g = make_thruster(dvec3(0, 0, -4), 1000.0, /*limit*/ 0.05);
    const dvec3 a0 = gimbal_deflected_axis(g, dvec2(0, 0));
    CHECK(approx3(a0, dvec3(0, 0, -1)));
    const dvec3 a1 = gimbal_deflected_axis(g, dvec2(0.1, 0.0));
    // deflection magnitude clamped 0.1 -> 0.05 exactly about +X:
    const double cosang = dot(a1, dvec3(0, 0, -1));  // == cos(0.05)? rotate about x by -? verify numerically
    CHECK(std::fabs(std::acos(std::clamp(cosang, -1.0, 1.0)) - 0.05) < 1e-12);
    const dvec3 a2 = gimbal_deflected_axis(g, dvec2(10.0, 10.0));  // huge input -> clamped
    const double cos2 = dot(a2, dvec3(0, 0, -1));
    CHECK(std::fabs(std::acos(std::clamp(cos2, -1.0, 1.0)) - 0.05) < 1e-5);  // two-axis compose: O(theta^3)
    // fixed mount (limit 0) ignores gimbal entirely:
    const dvec3 a3 = gimbal_deflected_axis(make_thruster(dvec3(), 1.0, 0.0), dvec2(0.3, 0.3));
    CHECK(approx3(a3, dvec3(0, 0, -1)));
    // degenerate axis -> zero axis, no force, no record:
    auto dz = apply_thrusters(dquat{}, {}, {make_thruster(dvec3(1, 2, 3), 500.0, 0.0, dvec3(0, 0, 0))},
                              {thruster_input{1.0, dvec2(0, 0)}});
    CHECK(dz.applied.empty());
    CHECK(approx3(dz.force_world, dvec3(0, 0, 0)));

    // ---- 4. attitude rotates the world force (yaw 90: -Z -> -X) ----
    auto yaw = apply_thrusters(quat_yaw(math::kHalfPi_d), dvec3(0.5, 0, 0), ts,
                               {thruster_input{1.0, dvec2(0, 0)}});
    CHECK(std::fabs(yaw.force_world.x + 1000.0) < 1e-9);
    CHECK(std::fabs(yaw.force_world.y) < 1e-9);
    CHECK(std::fabs(yaw.force_world.z) < 1e-9);
    // torque stays in BODY axes (frame-independent of att):
    CHECK(approx3(yaw.torque_body, dvec3(0, -500, 0)));

    // ---- 5. one real integrate_step tick: engine off-CoM -> omega grows ----
    rigid_params p;
    p.mass = 100.0;
    p.inertia_diag = dvec3(100, 100, 100);
    rigid_state s;
    const auto tf = apply_thrusters(s.att, dvec3(0.5, 0, 0), ts, {thruster_input{1.0, dvec2(0, 0)}});
    integrate_step(s, p, tf.force_world / p.mass, tf.torque_body);
    CHECK(s.vel.z == doctest::Approx(-10.0 / 60.0));          // a*dt = (1000/100)/60
    CHECK(s.omega_body.y == doctest::Approx(-5.0 / 60.0));    // tau/I*dt = 500/100/60 (RK2 first step exact)
    CHECK(state_ok(s));
}
