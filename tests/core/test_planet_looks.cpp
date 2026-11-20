// BOILERPLATE pins for the render-systems interlude decisions (#57 terrain
// look extensions, #59 atmosphere rendering, #61 composition-driven optics,
// #62 authored weather) plus a WFC probe for the UNRATIFIED prefab-base idea
// (proposed #63). Self-contained by design: the algorithms live in this TU as
// throwaway reference ports so the math and determinism contract can be
// proven (here and on MSVC/Windows, decision #38) before any of it gets
// engine homes, checklists, and cooklets. When the real implementations land,
// these cases graduate into production-header tests and this file shrinks.
//
// The exact algorithm block below was validated end-to-end in a Linux
// ASan/UBSan scratch harness (ALL PASS, 2026-11-20). The formal ctest run of
// THIS file happens on the MSI/Windows builder first (user's call) — do not
// treat "built" as "passed" until it has.
//
// Keep MSVC /W4 /WX and GCC -Wall -Wextra -Wpedantic -Werror happy: explicit
// integer/float types, no M_PI, ordered containers, no unused locals.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Deterministic integer mixing (splitmix64 family). Chosen over std::mt19937
// because the pin we ultimately care about is cross-compiler bit-identity of
// *sample sequences* — mt19937 params are standard, but the mixing keeps the
// sampler code itself tiny and auditable, matching engine RNG discipline.
// ---------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;  // M_PI is not standard (MSVC)

uint64_t mix64(uint64_t z) {
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
struct Lcg {  // ordered, seeded, tiny; Knuth MMIX constants
    uint64_t s;
    uint64_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return s >> 33; }
};

// ---------------------------------------------------------------------------
// #57: terrain sampler look extensions — value-noise fBm with a smooth vs
// ridged octave mode and a domain-warp knob. fp64 (engine sampler law).
// ---------------------------------------------------------------------------
double lattice_value(int64_t x, int64_t y, uint64_t seed) {
    uint64_t h = mix64(mix64(seed + static_cast<uint64_t>(x)) ^
                       (static_cast<uint64_t>(y) * 0xc2b2ae3d27d4eb4full));
    return static_cast<double>(h >> 11) / 9007199254740992.0;  // [0,1)
}
double smoothstep01(double t) { return t * t * (3.0 - 2.0 * t); }
double value_noise(double px, double py, double freq, uint64_t seed) {
    double x = px * freq, y = py * freq;
    auto ix = static_cast<int64_t>(std::floor(x));  // explicit type: a
    auto iy = static_cast<int64_t>(std::floor(y));  // decltype of the cast trips MSVC
    double fx = x - static_cast<double>(ix), fy = y - static_cast<double>(iy);
    double a = lattice_value(ix,     iy,     seed);
    double b = lattice_value(ix + 1, iy,     seed);
    double c = lattice_value(ix,     iy + 1, seed);
    double d = lattice_value(ix + 1, iy + 1, seed);
    double u = smoothstep01(fx), v = smoothstep01(fy);
    double ab = a + (b - a) * u, cd = c + (d - c) * u;
    return ab + (cd - ab) * v;                        // [0,1)
}
// fbm over octaves; ridged flips each octave's shape to 1-|2n-1| (peaks at
// the noise's zero crossings — Lague-style ridges, re-derived here).
// warp samples the stack at p + w*noiseVec(p) on the SAME seeded lattice.
double fbm_height(double px, double py, uint64_t seed, bool ridged, double warp,
                  int octaves, double freq, double lacunarity, double gain) {
    double warp_x = 0.0, warp_y = 0.0;
    if (warp != 0.0) {
        warp_x = value_noise(px, py, freq, seed ^ 0xa5a5ull) * 2.0 - 1.0;
        warp_y = value_noise(px, py, freq, seed ^ 0x5a5aull) * 2.0 - 1.0;
        px += warp * warp_x;
        py += warp * warp_y;
    }
    double sum = 0.0, amp = 1.0, f = freq, norm = 0.0;
    for (int o = 0; o < octaves; ++o) {
        double n = value_noise(px, py, f, seed + static_cast<uint64_t>(o) * 0x1000193ull);
        n = n * 2.0 - 1.0;                            // [-1,1)
        if (ridged) n = 1.0 - std::fabs(2.0 * n);     // ridge transform
        sum += amp * n;
        norm += amp;
        amp *= gain;
        f *= lacunarity;
    }
    return sum / norm;                                // ~[-1,1]
}
uint64_t grid_hash(const std::vector<double>& samples) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (double s : samples) {
        uint64_t bits = 0;
        std::memcpy(&bits, &s, sizeof(bits));
        h = (h ^ bits) * 0x100000001b3ull;
    }
    return h;
}
std::vector<double> sample_grid(uint64_t seed, bool ridged, double warp) {
    std::vector<double> out;
    out.reserve(64 * 64);
    for (int gy = 0; gy < 64; ++gy)
        for (int gx = 0; gx < 64; ++gx)
            out.push_back(fbm_height(static_cast<double>(gx) / 64.0,
                                     static_cast<double>(gy) / 64.0,
                                     seed, ridged, warp, 6, 8.0, 2.0, 0.5));
    return out;
}

// ---------------------------------------------------------------------------
// #61: composition-derived optics. Rayleigh cross-sections relative to N2 at
// 550 nm, per molecule, from squared molecular polarizability volumes
// (~public tables: N2 1.76, O2 1.59, CO2 2.65, Ar 1.64, CH4 2.59, H2 0.79 in
// 1e-24 cm^3); mass in amu. Three sampled wavelengths only (650/550/450 nm) —
// full spectral is explicitly out per #61.
// ---------------------------------------------------------------------------
struct Gas { const char* name; double alpha_rel2; double mass_amu; };  // (a/a_N2)^2
const Gas kGasTable[] = {
    {"N2",  1.000, 28.014}, {"O2", 0.812, 31.998}, {"CO2", 2.269, 44.009},
    {"Ar",  0.867, 39.948}, {"CH4", 2.163, 16.043}, {"H2", 0.202,  2.016},
};
const double kWl[3] = {650.0, 550.0, 450.0};  // nm; R G B sampling points

bool resolve_gas(const std::string& name, Gas& out) {
    for (const Gas& g : kGasTable)
        if (name == g.name) { out = g; return true; }
    return false;
}
// mole fractions -> SURFACE VERTICAL OPTICAL DEPTH per channel + mean mass:
//   tau_c = column * sum_i x_i * sigma_abs_i * (550/l_c)^4
//   column = p_s / (m_mean * g)   (hydrostatic: every body's column follows
//   from surface pressure and molecular weight — 2.1e29 m^-2 for Earth).
// sigma unit 4.767e-31 m^2 per alpha_rel2 unit at 550 nm: calibrated so the
// Earth standard mix lands on the published tau_550 = 0.098 (Bucholtz-style
// Rayleigh table; boilerplate constant, real engine derives from index data).
bool mixture_optics(const std::vector<std::pair<std::string, double>>& fractions,
                    double p_pa, double g_ms2,
                    double sigma_mix[3], double& mean_mass_amu, double tol = 1e-9) {
    double sum = 0.0;
    for (auto& fr : fractions) sum += fr.second;
    if (std::fabs(sum - 1.0) > tol) return false;     // #61 cook validation rule
    sigma_mix[0] = sigma_mix[1] = sigma_mix[2] = 0.0;
    mean_mass_amu = 0.0;
    for (auto& fr : fractions) {
        Gas g;
        if (!resolve_gas(fr.first, g)) return false;  // #61: unknown gas name fails
        mean_mass_amu += fr.second * g.mass_amu;
        for (int c = 0; c < 3; ++c)
            sigma_mix[c] += fr.second * g.alpha_rel2 *
                            std::pow(550.0 / kWl[c], 4.0);
    }
    double column = p_pa / (mean_mass_amu * 1.66053906660e-27 * g_ms2);
    for (int c = 0; c < 3; ++c) sigma_mix[c] *= column * 4.767e-31;
    return true;
}
// derived scale height, #61: H = kT/(m g), m from the mix (amu -> kg).
double derived_scale_height(double T_k, double mean_mass_amu, double g_m_s2) {
    const double k_boltzmann = 1.380649e-23;
    const double amu_kg = 1.66053906660e-27;
    return k_boltzmann * T_k / (mean_mass_amu * amu_kg * g_m_s2);
}

// ---------------------------------------------------------------------------
// #59: single-scatter sky march. Exponential density rho = exp(-h/H); view
// ray marched 16 samples through the spherical shell; Rayleigh phase
// (1+cos^2 xi)/4 (dipole, un-normalized — only comparisons matter here);
// optional well-mixed aerosol with Henyey-Greenstein lobe. I0 = 1 per channel.
// The SUN path uses Kasten-Young relative air mass — sunset red comes from
// the direct beam's long low-elevation slant, not from the view ray.
// ---------------------------------------------------------------------------
struct Rgb { double r, g, b; };
double hg_phase(double cos_xi, double g_hg) {  // un-normalized lobe shape
    double d = 1.0 + g_hg * g_hg - 2.0 * g_hg * cos_xi;
    return (1.0 - g_hg * g_hg) / std::pow(d, 1.5);
}
Rgb sky_radiance(double theta_rad, double phi_rad, double sun_elev_rad,
                 const double sigma[3], double haze = 0.0, double g_hg = 0.7) {
    const double R = 6.371e6, H = 8434.0;             // Earth-ish pins
    double st = std::sin(theta_rad), ct = std::cos(theta_rad);
    double dir[3] = {st * std::cos(phi_rad), st * std::sin(phi_rad), ct};
    double sun[3] = {std::cos(sun_elev_rad), 0.0, std::sin(sun_elev_rad)};
    double cos_xi = dir[0] * sun[0] + dir[1] * sun[1] + dir[2] * sun[2];
    double phase_ray = 0.25 * (1.0 + cos_xi * cos_xi);
    double phase_mie = hg_phase(cos_xi, g_hg);
    double t_exit = -R * ct + std::sqrt(R * ct * R * ct + 2.0 * R * (8.0 * H) + (8.0 * H) * (8.0 * H));
    double tau_view[3] = {0.0, 0.0, 0.0};
    Rgb L{0.0, 0.0, 0.0};
    double elev_deg = sun_elev_rad * 180.0 / kPi;
    double am = 1.0 / (std::sin(sun_elev_rad) + 0.50090976 * std::pow(elev_deg + 0.60699737, -1.21818));
    for (int s = 0; s < 16; ++s) {
        double t = (static_cast<double>(s) + 0.5) * t_exit / 16.0;
        double r_here = std::sqrt(R * R + 2.0 * R * ct * t + t * t);
        double h = r_here - R;
        double rho = std::exp(-h / H);
        double step = t_exit / 16.0;
        double dn = rho * step / H;                     // H-normalized column
        for (int c = 0; c < 3; ++c) {
            double t_sun_c = std::exp(-sigma[c] * rho * am);
            double trans = t_sun_c * std::exp(-tau_view[c]);
            double ray = sigma[c] * dn * phase_ray * trans;
            double aer = haze * sigma[c] * dn * phase_mie * trans;
            if (c == 0) L.r += ray + aer;
            if (c == 1) L.g += ray + aer;
            if (c == 2) L.b += ray + aer;
            tau_view[c] += (sigma[c] + haze * sigma[c]) * dn;
        }
    }
    return L;
}
// orbital limb: chord optical depth through the shell for a grazing ray with
// minimum altitude h_min (camera far above; straight-line chord). Returned in
// metres-of-density; shader multiplies by sigma_mix[c] / H for dimensionless
// slant optical depth.
double limb_tau(double h_min, double shell_top, double H) {
    const double R = 6.371e6;
    double b = R + h_min;
    if (b >= R + shell_top) return 0.0;
    double tmax = std::sqrt((R + shell_top) * (R + shell_top) - b * b);
    double step = 2.0 * tmax / 32.0;
    double tau = 0.0;
    for (int s = 0; s < 32; ++s) {
        double t = -tmax + (static_cast<double>(s) + 0.5) * step;
        double h = std::sqrt(b * b + t * t) - R;
        tau += std::exp(-h / H) * step;
    }
    return tau;
}

// ---------------------------------------------------------------------------
// #62: authored weather — state is a pure function of (seed, region, tick):
// scheduled events replay identically under any sampling cadence, so a
// 1000x time-compressed run agrees with the 1x run at shared ticks (the
// replay-hash-safe property #62 pins).
// ---------------------------------------------------------------------------
enum WeatherState : uint8_t { kCalm = 0, kWind = 1, kRain = 2, kStorm = 3 };
struct WeatherRegion {
    // ordered transition table per current state -> [calm,wind,rain,storm] weights
    static constexpr int kNext[4][4] = {
        {0, 5, 3, 1}, {4, 0, 4, 2}, {3, 4, 0, 3}, {2, 5, 4, 0},
    };
    std::vector<uint64_t> event_ticks;  // ordered
    std::vector<uint8_t>  event_state;
    void build(uint64_t seed, uint64_t region, uint64_t horizon) {
        Lcg rng{mix64(seed ^ (region * 0x9e3779b9ull))};
        uint64_t t = rng.next() % 2000ull;
        uint8_t state = kCalm;
        while (t < horizon) {
            event_ticks.push_back(t);
            event_state.push_back(state);
            t += 500ull + rng.next() % 1500ull;
            uint64_t roll = rng.next() % 10ull;
            uint8_t next = state;
            uint64_t acc = 0;
            for (int s = 0; s < 4; ++s) {
                acc += static_cast<uint64_t>(kNext[state][s]);
                if (roll < acc) { next = static_cast<uint8_t>(s); break; }
            }
            state = next;
        }
    }
    uint8_t state_at(uint64_t t) const {  // binary search over ordered events
        size_t lo = 0, hi = event_ticks.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (event_ticks[mid] <= t) lo = mid + 1; else hi = mid;
        }
        return lo == 0 ? static_cast<uint8_t>(kCalm) : event_state[lo - 1];
    }
};

// ---------------------------------------------------------------------------
// WFC probe (proposed #63, UNRATIFIED): edge-matching collapse of prefab
// tiles over an ordered grid. Determinism: seeded LCG, min-entropy with
// lowest-index tie-break, scan-order selection, ordered propagation stack,
// contradiction/checker failure -> deterministic re-roll (seed+attempt).
// Grammar in spike shape: 1 = open port kind C, 0 = wall (seals against
// other walls ONLY — wall-vs-port is illegal), 3 = airlock-outer (pairs with
// the site border and nothing else). Checker: >=1 airlock, >=1 power, >=2 hab,
// every room reachable from an airlock through matched open ports (DFS).
// Output is a flat string so run-vs-run equality is a plain string compare.
// ---------------------------------------------------------------------------
struct WfcTile { const char* name; uint8_t edge[4]; int w; int roomcls; char glyph; };
// roomcls: 0 corridor/none, 1 hab, 2 power, 3 airlock
std::vector<WfcTile> wfc_kit(bool include_airlock) {
    std::vector<WfcTile> t = {
        {"corr_x",  {1,1,1,1}, 3, 0, '+'},
        {"corr_ns", {1,0,1,0}, 2, 0, '|'},
        {"corr_we", {0,1,0,1}, 2, 0, '-'},
        {"corr_en", {1,1,0,0}, 2, 0, 'L'},
        {"corr_es", {0,1,1,0}, 2, 0, '7'},
        {"corr_sw", {0,0,1,1}, 2, 0, 'F'},
        {"corr_ws", {1,0,0,1}, 2, 0, 'J'},
        {"hab2",    {1,1,0,0}, 2, 1, 'H'},
        {"hab4",    {1,1,1,1}, 1, 1, '#'},
        {"power",   {1,0,0,0}, 2, 2, 'P'},
    };
    if (include_airlock) t.push_back({"airlock", {3,1,0,0}, 2, 3, 'X'});
    return t;
}
bool wfc_match(uint8_t a, uint8_t b) {
    return a == b;  // kinds pair by equality: wall(0)-wall(0) is legal, wall-port is not
}
bool wfc_generate(uint64_t seed, bool include_airlock, std::string& layout_out) {
    const int W = 10, H = 10;
    auto kit = wfc_kit(include_airlock);
    const int T = static_cast<int>(kit.size());
    for (int attempt = 0; attempt < 128; ++attempt) {
        Lcg rng{mix64(seed + static_cast<uint64_t>(attempt) * 7919ull)};
        std::vector<std::vector<bool>> ok(static_cast<size_t>(W * H),
                                         std::vector<bool>(static_cast<size_t>(T), true));
        auto cnt = [&](int i) { int n = 0; for (int t = 0; t < T; ++t) if (ok[(size_t)i][(size_t)t]) ++n; return n; };
        // border prune: edges leaving the site must be wall(0) or airlock-outer(3)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int t = 0; t < T; ++t) {
                    if (y == 0   && kit[(size_t)t].edge[0] != 0 && kit[(size_t)t].edge[0] != 3) ok[(size_t)(y * W + x)][(size_t)t] = false;
                    if (y == H-1 && kit[(size_t)t].edge[2] != 0 && kit[(size_t)t].edge[2] != 3) ok[(size_t)(y * W + x)][(size_t)t] = false;
                    if (x == W-1 && kit[(size_t)t].edge[1] != 0 && kit[(size_t)t].edge[1] != 3) ok[(size_t)(y * W + x)][(size_t)t] = false;
                    if (x == 0   && kit[(size_t)t].edge[3] != 0 && kit[(size_t)t].edge[3] != 3) ok[(size_t)(y * W + x)][(size_t)t] = false;
                    // no other tile carries kind 3, so an A port facing the site
                    // interior could never propagate a match: forbid it outright.
                    if (y > 0   && kit[(size_t)t].edge[0] == 3) ok[(size_t)(y * W + x)][(size_t)t] = false;
                    if (y < H-1 && kit[(size_t)t].edge[2] == 3) ok[(size_t)(y * W + x)][(size_t)t] = false;
                    if (x < W-1 && kit[(size_t)t].edge[1] == 3) ok[(size_t)(y * W + x)][(size_t)t] = false;
                    if (x > 0   && kit[(size_t)t].edge[3] == 3) ok[(size_t)(y * W + x)][(size_t)t] = false;
                }
        auto propagate = [&](int idx) -> bool {
            std::vector<int> stack{idx};
            const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0}, opp[4] = {2, 3, 0, 1};
            while (!stack.empty()) {
                int cur = stack.back(); stack.pop_back();
                int cx = cur % W, cy = cur / W;
                for (int d = 0; d < 4; ++d) {
                    int nx = cx + dx[d], ny = cy + dy[d];
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    int nidx = ny * W + nx;
                    bool changed = false;
                    for (int i = 0; i < T; ++i) if (ok[(size_t)nidx][(size_t)i]) {
                        bool any = false;
                        for (int j = 0; j < T; ++j) if (ok[(size_t)cur][(size_t)j] &&
                            wfc_match(kit[(size_t)j].edge[d], kit[(size_t)i].edge[opp[d]])) { any = true; break; }
                        if (!any) { ok[(size_t)nidx][(size_t)i] = false; changed = true; }
                    }
                    if (changed) {
                        if (cnt(nidx) == 0) return false;
                        stack.push_back(nidx);
                    }
                }
            }
            return true;
        };
        bool contradiction = false;
        for (int i = 0; i < W * H && !contradiction; ++i) if (!propagate(i)) contradiction = true;
        while (!contradiction) {
            int best = -1, be = 1 << 30;
            for (int i = 0; i < W * H; ++i) { int n = cnt(i); if (n > 1 && n < be) { be = n; best = i; } }
            if (best < 0) break;
            int n = cnt(best);
            int pick = static_cast<int>(rng.next() % static_cast<uint64_t>(n));
            int chosen = -1;
            for (int t = 0; t < T; ++t) if (ok[(size_t)best][(size_t)t]) { if (pick-- == 0) { chosen = t; break; } }
            for (int t = 0; t < T; ++t) ok[(size_t)best][(size_t)t] = (t == chosen);
            if (!propagate(best)) { contradiction = true; break; }
        }
        if (contradiction) continue;
        std::vector<int> sol(static_cast<size_t>(W * H), -1);
        for (int i = 0; i < W * H; ++i)
            for (int t = 0; t < T; ++t) if (ok[(size_t)i][(size_t)t]) { sol[(size_t)i] = t; break; }
        // checker: contract counts + connectivity
        int airlock = -1, power = 0, hab = 0;
        for (int i = 0; i < W * H; ++i) {
            int cls = kit[(size_t)sol[(size_t)i]].roomcls;
            if (cls == 1) ++hab;
            else if (cls == 2) ++power;
            else if (cls == 3 && airlock < 0) airlock = i;
        }
        bool connected = (airlock >= 0);
        if (connected) {
            std::vector<bool> vis(static_cast<size_t>(W * H), false);
            std::vector<int> stack{airlock};
            vis[(size_t)airlock] = true;
            const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0}, opp[4] = {2, 3, 0, 1};
            while (!stack.empty()) {
                int cur = stack.back(); stack.pop_back();
                int cx = cur % W, cy = cur / W;
                for (int d = 0; d < 4; ++d) {
                    int nx = cx + dx[d], ny = cy + dy[d];
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    int ni = ny * W + nx;
                    if (vis[(size_t)ni]) continue;
                    if (wfc_match(kit[(size_t)sol[(size_t)cur]].edge[d],
                                  kit[(size_t)sol[(size_t)ni]].edge[opp[d]])) {
                        vis[(size_t)ni] = true;
                        stack.push_back(ni);
                    }
                }
            }
            for (int i = 0; i < W * H; ++i)
                if (kit[(size_t)sol[(size_t)i]].roomcls > 0 && !vis[(size_t)i]) { connected = false; break; }
        }
        if (airlock >= 0 && power >= 1 && hab >= 2 && connected) {
            layout_out.clear();
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) layout_out.push_back(kit[(size_t)sol[(size_t)(y * W + x)]].glyph);
                layout_out.push_back('\n');
            }
            return true;
        }
    }
    return false;  // re-rolls exhausted — checker biting (bad kit)
}

}  // namespace

TEST_CASE("#57 terrain look extensions: determinism + knob orthogonality") {
    uint64_t seed = 0xB16B00B5ull;
    auto legacy  = sample_grid(seed, false, 0.0);
    auto legacy2 = sample_grid(seed, false, 0.0);
    auto ridged  = sample_grid(seed, true, 0.0);
    auto warped  = sample_grid(seed, false, 0.6);
    auto diff    = sample_grid(seed ^ 1, false, 0.0);
    CHECK(grid_hash(legacy) == grid_hash(legacy2));
    CHECK(grid_hash(ridged) != grid_hash(legacy));
    CHECK(grid_hash(warped) != grid_hash(legacy));
    CHECK(grid_hash(diff) != grid_hash(legacy));
    for (double h : legacy) { CHECK(h >= -1.0001); CHECK(h <= 1.0001); }
}

TEST_CASE("#61 composition-driven optics: mixture -> optical depth + scale height") {
    double sig[3], mass = 0.0;
    CHECK(mixture_optics({{"N2",0.7808},{"O2",0.2095},{"Ar",0.0093},{"CO2",0.0004}}, 101325.0, 9.81, sig, mass));
    CHECK(mass == doctest::Approx(28.96).epsilon(0.001));
    CHECK(sig[2] / sig[0] == doctest::Approx(4.353));  // (650/450)^4 = 4.3531
    CHECK(sig[1] == doctest::Approx(0.098).epsilon(0.12));  // Earth tau_550 anchor
    CHECK(derived_scale_height(288.0, mass, 9.81) == doctest::Approx(8430.0).epsilon(0.01));

    double s2[3], m2 = 0.0;
    CHECK_FALSE(mixture_optics({{"N2",0.5},{"O2",0.4}}, 101325.0, 9.81, s2, m2));  // sum != 1 -> cook fail
    CHECK_FALSE(mixture_optics({{"N2",0.5},{"HF",0.5}}, 101325.0, 9.81, s2, m2));  // unknown gas -> cook fail

    double sM[3], mM = 0.0;
    CHECK(mixture_optics({{"CO2",0.9557},{"N2",0.027},{"Ar",0.016},{"O2",0.0013}}, 610.0, 3.71, sM, mM));
    CHECK(derived_scale_height(210.0, mM, 3.71) == doctest::Approx(11000.0).epsilon(0.03));
    CHECK(sM[1] < 0.005);  // p_s=610 Pa: Mars Rayleigh near-black; dust (haze) dominates its sky

    double sH[3], mH = 0.0;
    CHECK_FALSE(mixture_optics({{"H2",0.84},{"He",0.0}}, 1e5, 10.0, sH, mH));  // He not in table yet: cook fail
    CHECK(mixture_optics({{"H2",0.84},{"CH4",0.16}}, 1e5, 10.0, sH, mH));
    double h_earth = derived_scale_height(288.0, mass, 9.81);
    CHECK(derived_scale_height(150.0, mH, 10.0) > 3.0 * h_earth);  // H2 world: fat envelope

    double sC[3], mC = 0.0;
    CHECK(mixture_optics({{"CO2",1.0}}, 101325.0, 9.81, sC, mC));
    CHECK(sC[2] > sig[2]);  // same column: CO2 scatters harder than N2/Earth mix
}

TEST_CASE("#59 sky scattering: day blue, sunset red, aerosol lobe, airless black") {
    double sig[3], mass = 0.0;
    REQUIRE(mixture_optics({{"N2",0.7808},{"O2",0.2095},{"Ar",0.0093},{"CO2",0.0004}}, 101325.0, 9.81, sig, mass));
    double sun = 30.0 * kPi / 180.0;
    auto z  = sky_radiance(0.0, 0.0, sun, sig);
    auto hz = sky_radiance(88.0 * kPi / 180.0, 0.0, sun, sig);
    CHECK(z.b > z.r);   // day zenith: blue
    CHECK(hz.b > hz.r); // 30-deg-sun horizon still blue-ish (single scatter keeps hue;
                        // reddening needs a LOW sun, pinned next — not a 30-deg horizon)
    double sun2 = 3.0 * kPi / 180.0;
    auto s_zen = sky_radiance(0.0, 0.0, sun2, sig);
    auto s_hz  = sky_radiance(80.0 * kPi / 180.0, 0.0, sun2, sig);
    CHECK(s_hz.r > 0.0);
    CHECK(s_hz.b > 0.0);
    CHECK(s_hz.b / s_hz.r < s_zen.b / s_zen.r);  // sunset: toward-sun horizon reddens vs up
    auto ss = sky_radiance(80.0 * kPi / 180.0, 0.0,   sun, sig, 0.02);
    auto as = sky_radiance(80.0 * kPi / 180.0, kPi,   sun, sig, 0.02);
    CHECK(ss.r + ss.g + ss.b > as.r + as.g + as.b);  // Mie lobe: sunward brighter
    double vs[3] = {0.0, 0.0, 0.0};
    auto bare = sky_radiance(0.0, 0.0, sun, vs);
    CHECK(bare.r == 0.0);  // exact: zero cross-section -> exact zero radiance
    CHECK(bare.b == 0.0);
}

TEST_CASE("#59 orbital limb: grazing chord optical depth") {
    double H = 8434.0, top = 8.0 * H;
    double tl = limb_tau(5e3,  top, H);
    double tm = limb_tau(30e3, top, H);
    double th = limb_tau(top - 100.0, top, H);
    CHECK(tl > tm);
    CHECK(tm > th);              // deeper chord = denser shell = more optical depth
    CHECK(th < 0.01 * tl);       // the glow hugs the limb: reentry look comes from grazing
}

TEST_CASE("#62 authored weather: pure (seed, region, tick) replay invariant") {
    WeatherRegion a, b, c;
    a.build(0xC0FFEE, 3, 200000);
    b.build(0xC0FFEE, 3, 200000);
    c.build(0xBEEF01, 3, 200000);
    CHECK(a.event_ticks.size() == b.event_ticks.size());
    bool same = true;
    for (uint64_t t = 0; t < 200000; t += 1000)  // 1000x-compressed sampling
        if (a.state_at(t) != b.state_at(t)) same = false;
    CHECK(same);
    CHECK(a.state_at(99999) == b.state_at(99999));
    bool dif = false;
    for (uint64_t t = 0; t < 200000; t += 4096)
        if (a.state_at(t) != c.state_at(t)) { dif = true; break; }
    CHECK(dif);                  // different seed: different weather history
    CHECK_FALSE(a.event_ticks.empty());
}

TEST_CASE("WFC prefab bases (proposed #63): deterministic collapse + checker") {
    std::string l1, l2, other, bad;
    bool g1 = wfc_generate(0x5EED42, true, l1);
    bool g2 = wfc_generate(0x5EED42, true, l2);
    bool go = wfc_generate(0xA11CE, true, other);
    bool gb = wfc_generate(0x5EED42, false, bad);
    CHECK(g1);
    CHECK(g2);
    CHECK(l1 == l2);             // same seed -> bit-identical layout
    CHECK(go);
    CHECK(other != l1);          // different seed -> different layout
    CHECK_FALSE(gb);             // kit without airlock: checker rejects every re-roll
    int X = 0, P = 0, Hh = 0;
    for (char ch : l1) { if (ch == 'X') ++X; if (ch == 'P') ++P; if (ch == 'H' || ch == '#') ++Hh; }
    CHECK(X >= 1);
    CHECK(P >= 1);
    CHECK(Hh >= 2);
}
