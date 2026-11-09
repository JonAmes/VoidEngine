// test: ship_designer_flight — 5.13 Phase-5 done-when harness (headless):
// cook the fixture hull (cooklet) -> load_hull_loadout -> assemble a
// designer loadout (hull + 2 engines + 2 wings) on the LOADED sockets.
// CoM + full inertia tensor are pinned against values hand-computed from
// the exact fixture anchor coordinates (24500/110000 etc. — every anchor
// is lattice-exact, so the arithmetic is exact rational). Misaligning one
// engine socket off the centerline produces a real (hand-pinned) yaw
// torque through apply_thrusters. The Phase-4 stabilizer assist is flown
// with gains DERIVED from the computed tensor (stab_kd = 5 * smallest
// principal inertia — not the hand-authored arcade constants); the decay
// matches the analytic discrete first-order law for those derived gains.
// The whole flight is replay-hash deterministic (per-tick FNV equality +
// interval hash + 1-ULP command canary). Numbers are console-printed per
// the done-when bullet.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "../ve_test_portability.h"  // getpid

#include <ve/cook/cook.h>
#include <ve/core/cooked.h>
#include <ve/core/sim/assist.h>
#include <ve/core/sim/inertia.h>
#include <ve/core/sim/loadout.h>
#include <ve/core/sim/loadout_load.h>
#include <ve/core/sim/rails.h>
#include <ve/core/sim/replay.h>
#include <ve/core/sim/rigid.h>
#include <ve/core/sim/thruster.h>
#include <ve/core/sim/time.h>
#include "ve/game/attr_keys.h"

#ifndef VE_SOURCE_DIR
#error "VE_SOURCE_DIR must be defined by CMake"
#endif

namespace {

using namespace ve::core;
using namespace ve::core::sim;
using math::dmat3;
using math::dquat;
using math::dvec3;

std::string tmp_root(const char* name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("ve_flight513_") + name + "_" + std::to_string(ve_test::pid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

// ---- hand-computed pins (exact rational arithmetic, see notes.md 5.13) ----
// Hull (authored #14/#42): m=98000, com=(0, 1/4, 2/5), I_diag=(420000,
// 980000, 1120000) about ITS com. Designer mounts (point masses):
// engines 2000 kg @ (+-2, 0, -6); wings 4000 kg @ (+-6, 0, 0).
// M = 110000. CoM_y = 98000/4 / 110000 = 24500/110000, CoM_z =
// (98000*2/5 - 12000 - 12000)/110000 = 15200/110000, CoM_x = 0.
constexpr double kMass = 110000.0;
constexpr double kComY = 24500.0 / 110000.0;
constexpr double kComZ = 15200.0 / 110000.0;
// Tensor about CoM, hull-local axes (parallel-axis PLUS the hull's own
// diag; symmetric x kills every xy/xz cross; the yz couple from the hull
// sitting off the mount plane is the only off-diagonal — column-major m[],
// yz lives at m[5]/m[7]):
constexpr double kIxx = 6360726.0 / 11.0;    // 578247.8181818182
constexpr double kIyy = 15857376.0 / 11.0;   // 1441579.6363636365
constexpr double kIzz = 15671350.0 / 11.0;   // 1424668.1818181819
constexpr double kIyz = -70560.0 / 11.0;     // -6414.545454545455
// Principal inertias ascending (yz block quadratic; x decoupled):
constexpr double kPr1 = 578247.8181818182;
constexpr double kPr2 = 1422510.4404260865;
constexpr double kPr3 = 1443737.3777557318;
// Twin full burn (both engines, axis -Z, 400 kN each): CoM sits 24500/110000
// m ABOVE the engine line -> pure pitch torque 2*com_y*F about x.
constexpr double kThrustN = 400000.0;
constexpr double kPitchTwin = 2.0 * kComY * kThrustN;  // 178181.81818181818
// Misaligned variant: engine_L moved to x=+4 (on-lattice). New com_x =
// (2000*4 - 2000*2)/110000 = 4000/110000; yaw torque = -sum(r_x)*F_z =
// 400000*((4-cx)+(-2-cx)) with 2*cx = 8000/110000:
constexpr double kComXvar = 4000.0 / 110000.0;
constexpr double kYawVar = kThrustN * (2.0 - 2.0 * kComXvar);  // 770909.0909090909

// ---- harness glue (game-side): loadout -> mass_point list ----
std::vector<mass_point> parts_from(const Loadout& lo, std::string_view mass_key) {
    std::vector<mass_point> parts;
    const hull_info& h = lo.hull();
    mass_point hull{};
    hull.mass = h.hull_mass;
    hull.pos = h.hull_com_local;
    hull.local_tensor = dmat3::diag(h.hull_inertia_diag);
    parts.push_back(hull);
    for (const auto& mc : lo.mounts()) {
        double m = 0.0;
        for (const auto& a : mc.attrs)
            if (a.key == mass_key)
                m = a.value;
        const socket_desc* sock = nullptr;
        for (const auto& s : lo.sockets())
            if (s.id == mc.socket_id)
                sock = &s;
        REQUIRE(sock != nullptr);
        mass_point p{};
        p.mass = m;
        p.pos = sock->position_local;
        parts.push_back(p);  // point mass: zero local tensor
    }
    return parts;
}

const socket_desc* find_socket(const Loadout& lo, const std::string& id) {
    for (const auto& s : lo.sockets())
        if (s.id == id)
            return &s;
    return nullptr;
}

// Assemble the designer ship (hull + 2 engines + 2 wings) on copies of the
// LOADED fixture sockets. engine_L_on_off puts one engine 2 m off the
// centerline (on-lattice x=+4) -> the misalignment variant.
Loadout designer_ship(const hull_info& hull, bool engine_L_on_off) {
    std::vector<socket_desc> socks;
    for (const char* id : {"hp_engine_L", "hp_engine_R", "hp_wingtip_L", "hp_wingtip_R"}) {
        socket_desc s{};
        s.id = id;
        s.category = (std::string(id) == "hp_engine_L" || std::string(id) == "hp_engine_R")
                         ? socket_category::main_
                         : (std::string(id) == "hp_wingtip_R" ? socket_category::rcs_
                                                              : socket_category::utility_);
        // Fixture anchors are lattice-exact (5.9a): decode positions by name.
        if (s.id == "hp_engine_L") s.position_local = {2.0, 0.0, -6.0};
        if (s.id == "hp_engine_R") s.position_local = {-2.0, 0.0, -6.0};
        if (s.id == "hp_wingtip_L") s.position_local = {6.0, 0.0, 0.0};
        if (s.id == "hp_wingtip_R") s.position_local = {-6.0, 0.0, 0.0};
        socks.push_back(std::move(s));
    }
    {   // spare main hardpoint for the misalignment variant (on-lattice).
        socket_desc s{};
        s.id = "hp_engine_off";
        s.category = socket_category::main_;
        s.position_local = {4.0, 0.0, -6.0};
        socks.push_back(std::move(s));
    }
    std::vector<component_type> catalog = {
        {"type.designer.engine", socket_category::main_},
        {"type.designer.wing_l", socket_category::utility_},  // category pair
        {"type.designer.wing_r", socket_category::rcs_},      // is structural
    };
    const std::string eng_L = engine_L_on_off ? "hp_engine_off" : "hp_engine_L";
    std::vector<mount_request> mounts;
    for (auto [id, type, sock, m] : std::vector<std::tuple<std::string, std::string, std::string, double>>{
             {"eng_L", "type.designer.engine", eng_L, 2000.0},
             {"eng_R", "type.designer.engine", "hp_engine_R", 2000.0},
             {"wing_L", "type.designer.wing_l", "hp_wingtip_L", 4000.0},
             {"wing_R", "type.designer.wing_r", "hp_wingtip_R", 4000.0}}) {
        mount_request mr{};
        mr.instance_id = std::move(id);
        mr.type_id = std::move(type);
        mr.socket_id = std::move(sock);
        mr.attrs = {{std::string(ve::game::kAttr_mass), m}};
        mounts.push_back(std::move(mr));
    }
    auto lo = Loadout::assemble(hull, std::move(socks), std::move(catalog), std::move(mounts));
    REQUIRE_MESSAGE(lo.has_value(), static_cast<int>(lo.error()));
    return std::move(*lo);
}

// attitude angle between two orientations (rad) — relative-quaternion norm.
double att_delta(const dquat& a, const dquat& b) {
    const dquat rel = math::conjugate(a) * b;
    const double v = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
    return 2.0 * std::atan2(v, std::fabs(rel.w));  // q and -q same rotation
}

// ---- the scripted flight (identical for every replay leg) ----
// phase 1: deliberate disturbance — yaw torque 500 kN*m for 30 ticks, no
//          assist (the spin the stabilizer must then hold).
// phase 2: 600 ticks stabilizer ONLY (gains derived from the tensor).
// phase 3: 60 ticks full twin burn (body -Z) + twin pitch torque + assist
//          (unless no_assist_burn — the contrast leg).
constexpr int kSpin_ticks = 30;
constexpr int kStab_ticks = 600;
constexpr int kBurn_ticks = 60;
constexpr double kSpin_torque = 500000.0;

struct flight_result {
    rigid_state end{};
    std::vector<std::uint64_t> hashes;
    double w_after_spin = 0.0;
    double w_after_stab = 0.0;
    double stab_drift = 0.0;
    double vx_burn = 0.0;
};

flight_result fly(const ship_inertia& mass_props, bool no_assist_burn,
                  const assist_gains& derived) {
    rigid_params p{};
    p.mass = mass_props.total_mass;
    p.inertia_tensor = mass_props.tensor;

    const double burn_torque_x = kPitchTwin;  // hand-derived twin pitch bias
    ReplayLog log;
    for (int i = 0; i < kSpin_ticks; ++i) {
        tick_command cmd{};
        cmd.torque_body = {0.0, kSpin_torque, 0.0};
        log.append(cmd);
    }
    // phases 2-3 are control-law ticks: the stabilizer torque is sampled
    // from the pre-tick state (deterministic; the arcade assist_mask path
    // hardcodes kArcade_assist_gains, so the DERIVED-gain law is baked
    // into the command stream through the pure assist.h function).
    flight_result res{};
    rail_world w;  // empty: deep space, no wells -> zero gravity path
    rigid_state s{};
    dquat att_after_spin{};
    for (int i = 0; i < kSpin_ticks; ++i) {
        replay_step(w, log, static_cast<std::size_t>(i), i, p, s);
        res.hashes.push_back(state_hash(i, s));
    }
    res.w_after_spin = s.omega_body.y;
    att_after_spin = s.att;
    const std::size_t base = log.size();
    for (int i = 0; i < kStab_ticks; ++i) {
        tick_command cmd{};
        cmd.torque_body = stabilizer_torque(s, derived);
        log.append(cmd);
        replay_step(w, log, base + static_cast<std::size_t>(i),
                    kSpin_ticks + i, p, s);
        res.hashes.push_back(state_hash(kSpin_ticks + i, s));
    }
    res.w_after_stab = std::sqrt(math::dot(s.omega_body, s.omega_body));
    res.stab_drift = att_delta(att_after_spin, s.att);
    const std::size_t burn0 = log.size();
    for (int i = 0; i < kBurn_ticks; ++i) {
        tick_command cmd{};
        cmd.thrust_body = {0.0, 0.0, -kThrustN * 2.0};
        cmd.torque_body = {burn_torque_x, 0.0, 0.0};
        if (!no_assist_burn)
            cmd.torque_body = cmd.torque_body + stabilizer_torque(s, derived);
        log.append(cmd);
        replay_step(w, log, burn0 + static_cast<std::size_t>(i),
                    kSpin_ticks + kStab_ticks + i, p, s);
        res.hashes.push_back(state_hash(kSpin_ticks + kStab_ticks + i, s));
    }
    res.vx_burn = std::fabs(s.vel.x);
    res.end = s;
    return res;
}

}  // namespace

TEST_CASE("ship_designer_flight") {
    // ================= A: cook the fixture hull -> load -> sockets ========
    const std::string src = tmp_root("src");
    const std::string out = tmp_root("out");
    std::filesystem::create_directories(src + "/samples");
    std::filesystem::create_directories(src + "/defs");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/samples/test_ship.glb",
                          src + "/samples/test_ship.glb");
    std::filesystem::copy(std::string(VE_SOURCE_DIR) + "/assets/defs/test_ship.def.toml",
                          src + "/defs/test_ship.def.toml");
    auto cres = ve::cook::cook_incremental(src, out);
    REQUIRE_MESSAGE(cres.ok, cres.error);

    std::string err;
    ve::core::cooked::Unit def0, sock;
    REQUIRE(ve::cook::read_unit_file(out + "/units/defs/test_ship.def.toml.ck", def0, &err));
    REQUIRE(ve::cook::read_unit_file(out + "/units/samples/test_ship.glb.sock.ck", sock, &err));
    auto loaded = load_hull_loadout(def0, sock, 0xA5EEDULL);
    REQUIRE_MESSAGE(loaded.has_value(), loaded.error());
    const Loadout& fixture = loaded->loadout;
    CHECK(fixture.hull().class_tag == "class.light_freighter");
    CHECK(fixture.hull().hull_mass == 98000.0);
    CHECK(fixture.hull().hull_inertia_diag ==
          dvec3(420000.0, 980000.0, 1120000.0));  // DEF0 passthrough exact
    CHECK(fixture.sockets().size() == 12);  // 8 hp + 4 interior anchors (#46a)
    CHECK(fixture.hull().mesh_content_id == 0xA5EEDULL);
    CHECK(loaded->mesh_unit_id == "samples/test_ship.glb");
    REQUIRE(find_socket(fixture, "hp_engine_L") != nullptr);
    CHECK(find_socket(fixture, "hp_engine_L")->position_local == dvec3(2.0, 0.0, -6.0));
    CHECK(find_socket(fixture, "hp_wingtip_L")->position_local == dvec3(6.0, 0.0, 0.0));
    CHECK(find_socket(fixture, "hp_wingtip_R")->position_local == dvec3(-6.0, 0.0, 0.0));

    // ================= B: designer ship — hand-computed mass properties ===
    const Loadout ship = designer_ship(fixture.hull(), false);
    auto parts = parts_from(ship, ve::game::kAttr_mass);
    REQUIRE(parts.size() == 5);
    auto ip = inertia_from_parts(parts);
    REQUIRE(ip.has_value());
    const ship_inertia& mi = *ip;
    CHECK(mi.total_mass == doctest::Approx(kMass));
    CHECK(mi.com_local.x == 0.0);  // exact cancellation: symmetric x pairs
    CHECK(mi.com_local.y == doctest::Approx(kComY).epsilon(1e-12));
    CHECK(mi.com_local.z == doctest::Approx(kComZ).epsilon(1e-12));
    const dmat3& I = mi.tensor;
    CHECK(I.m[0] == doctest::Approx(kIxx).epsilon(1e-12));
    CHECK(I.m[4] == doctest::Approx(kIyy).epsilon(1e-12));
    CHECK(I.m[8] == doctest::Approx(kIzz).epsilon(1e-12));
    CHECK(I.m[1] == 0.0);  // xy cross terms: exact cancellation
    CHECK(I.m[3] == 0.0);
    CHECK(I.m[2] == 0.0);  // xz cross terms: symmetric x cancels
    CHECK(I.m[6] == 0.0);
    CHECK(I.m[5] == doctest::Approx(kIyz).epsilon(1e-12));  // yz couple
    CHECK(I.m[7] == doctest::Approx(kIyz).epsilon(1e-12));
    const principal_frame pf = principal_axes(I);
    CHECK(pf.principal.x == doctest::Approx(kPr1).epsilon(1e-9));
    CHECK(pf.principal.y == doctest::Approx(kPr2).epsilon(1e-9));
    CHECK(pf.principal.z == doctest::Approx(kPr3).epsilon(1e-9));

    std::printf("ship_designer_flight: mass=%.0f com=(%.6f, %.6f, %.6f)\n",
                mi.total_mass, mi.com_local.x, mi.com_local.y, mi.com_local.z);
    std::printf("  tensor = [%.4f 0 0; 0 %.4f %.4f; 0 %.4f %.4f]  principal="
                "[%.4f, %.4f, %.4f]\n",
                I.m[0], I.m[4], I.m[5], I.m[7], I.m[8],
                pf.principal.x, pf.principal.y, pf.principal.z);

    // ================= C: misaligned engine -> real yaw torque ===========
    const Loadout ship_var = designer_ship(fixture.hull(), true);
    auto parts_var = parts_from(ship_var, ve::game::kAttr_mass);
    auto ipv = inertia_from_parts(parts_var);
    REQUIRE(ipv.has_value());
    CHECK(ipv->com_local.x == doctest::Approx(kComXvar).epsilon(1e-12));

    std::vector<thruster_static> thr(2);
    std::vector<thruster_input> in(2);
    for (int i = 0; i < 2; ++i) {
        const socket_desc* sd = find_socket(ship_var, i == 0 ? "hp_engine_off" : "hp_engine_R");
        REQUIRE(sd != nullptr);
        thr[i].position_local = sd->position_local;
        thr[i].axis_local = {0.0, 0.0, -1.0};
        thr[i].max_force = kThrustN;
        in[i].throttle = 1.0;
    }
    auto tf = apply_thrusters(dquat{}, ipv->com_local, thr, in);
    CHECK(tf.force_world.z == doctest::Approx(-2.0 * kThrustN).epsilon(1e-12));
    CHECK(tf.torque_body.x == doctest::Approx(kPitchTwin).epsilon(1e-9));
    CHECK(tf.torque_body.y == doctest::Approx(kYawVar).epsilon(1e-9));
    CHECK(tf.torque_body.z == 0.0);
    // symmetric baseline twin: the yaw cancels exactly, only the pitch
    // bias (CoM above the engine line) survives.
    thr[0].position_local = {2.0, 0.0, -6.0};
    auto tf0 = apply_thrusters(dquat{}, mi.com_local, thr, in);
    CHECK(tf0.torque_body.y == 0.0);
    CHECK(tf0.torque_body.x == doctest::Approx(kPitchTwin).epsilon(1e-9));
    std::printf("  twin torque symmetric=(%.2f, 0, 0)  misaligned=(%.2f, %.2f, 0)\n",
                tf0.torque_body.x, tf.torque_body.x, tf.torque_body.y);

    // ================= D: stabilizer, gains derived from the tensor ======
    // stab_kd = 5 * smallest principal inertia -> every axis damps at
    // >= 5 rad/s bandwidth. NOT the arcade 1e5 constant: for this 110 t
    // ship that constant would be a 4.6 s time constant on yaw.
    assist_gains g{};
    g.stab_kd = 5.0 * pf.principal.x;
    g.max_accel = 20.0;
    CHECK(g.stab_kd > 1.0e5);  // this ship genuinely needs the derived gain

    const flight_result a = fly(mi, false, g);
    // spin: y is not exactly a principal axis (Iyz couple) but the gyro
    // cross-feed is O(6414*w^2) ~ 1.9 kN*m — yaw law w0 = tau*t*(I^-1)_yy
    // with (I^-1)_yy = Izz/(Iyy*Izz - Iyz^2) = 6.936856e-7:
    CHECK(a.w_after_spin == doctest::Approx(250000.0 * 6.936856e-7).epsilon(1e-4));
    // 10 s of damping: omega annihilated, attitude drift bounded by the
    // first-order envelope w0/alpha, alpha = kd*(I^-1)_yy = 2.0056/s:
    CHECK(a.w_after_stab < 1e-8);
    CHECK(a.stab_drift == doctest::Approx(0.086466).epsilon(5e-2));
    CHECK(a.stab_drift < 0.1);
    // burn: thrust holds straight despite the pitch bias (quasi-steady
    // omega_x -> T/kd = 0.06163 rad/s; discrete approach 1-e^-5: 0.06121):
    CHECK(std::fabs(a.end.omega_body.x) == doctest::Approx(0.06121).epsilon(1e-2));
    CHECK(std::fabs(a.end.omega_body.y) < 0.05);
    CHECK(std::fabs(a.end.omega_body.z) < 0.05);
    CHECK(a.end.vel.z == doctest::Approx(-7.2727272727272725).epsilon(2e-2));
    CHECK(std::fabs(a.end.vel.x) < 1.0);
    CHECK(std::fabs(a.end.vel.y) < 0.5);
    // contrast: same burn WITHOUT the derived-gain assist — pitch omega
    // runs to T*1s/Ixx = 0.30814 and the flight veers in Y (the lateral X
    // component comes from the spin phase's yaw tilt and is assist-free
    // in both legs, so the pitch-plane veer is the honest contrast):
    const flight_result n = fly(mi, true, g);
    CHECK(std::fabs(n.end.omega_body.x) == doctest::Approx(178181.81818181818 / 578247.8181818182).epsilon(1e-4));
    CHECK(std::fabs(n.end.vel.y) > 2.0 * std::fabs(a.end.vel.y));
    std::printf("  stab kd=%.1f  w_spin=%.6f  drift=%.4f rad  burn w=%.4f v=(%.2f, %.2f, %.2f)\n",
                g.stab_kd, a.w_after_spin, a.stab_drift, a.end.omega_body.x,
                a.end.vel.x, a.end.vel.y, a.end.vel.z);
    std::printf("  no-assist contrast: w=%.4f lateral v_y=%.3f vs %.3f\n",
                n.end.omega_body.x, std::fabs(n.end.vel.y), std::fabs(a.end.vel.y));

    // ================= E: whole flight inside the replay hash ============
    const flight_result a2 = fly(mi, false, g);
    REQUIRE(a2.hashes.size() == a.hashes.size());
    CHECK(a2.hashes == a.hashes);
    CHECK(replay_interval_hash(a2.hashes, 0, a2.hashes.size() - 1) ==
          replay_interval_hash(a.hashes, 0, a.hashes.size() - 1));
    // canary: 1 ULP on one command torque must move the stamp.
    {
        // rebuild the log with the mutation (fly() owns its log; mutate the
        // 11th spin command by 1e-9 N*m and re-step by hand).
        rigid_params p{};
        p.mass = mi.total_mass;
        p.inertia_tensor = mi.tensor;
        ReplayLog log;
        for (int i = 0; i < kSpin_ticks; ++i) {
            tick_command cmd{};
            cmd.torque_body = {0.0, kSpin_torque, 0.0};
            log.append(cmd);
        }
        tick_command mut = log.at(10);
        mut.torque_body.y += 1e-9;
        log.set(10, mut);
        rail_world w;
        rigid_state s{};
        std::vector<std::uint64_t> hs;
        for (int i = 0; i < kSpin_ticks; ++i) {
            replay_step(w, log, static_cast<std::size_t>(i), i, p, s);
            hs.push_back(state_hash(i, s));
        }
        CHECK(replay_interval_hash(hs, 0, hs.size() - 1) !=
              replay_interval_hash(a.hashes, 0, static_cast<std::size_t>(kSpin_ticks) - 1));
    }
    std::printf("  replay interval hash = %016llx (690 ticks)\n",
                static_cast<unsigned long long>(
                    replay_interval_hash(a.hashes, 0, a.hashes.size() - 1)));
}
