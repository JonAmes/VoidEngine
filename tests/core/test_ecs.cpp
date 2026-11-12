// 8.2.5 — ecs_registry: the minimal ECS registry (PLAN #8). Pins the
// dispatch-index model (no RTTI), opaque names, slot+generation id hygiene,
// and the ordered tree view the 8.3 inspector walks.
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include "ve/core/ecs.h"

namespace {
struct test_alpha { int v = 0; };
struct test_beta { double w = 0.0; };
}  // namespace

TEST_CASE("ecs_registry") {
    ve::core::ecs_world w;

    // --- registration: index = order; same name -> same index -----------
    const auto alpha = w.register_component_type("ve.test/alpha");
    const auto beta = w.register_component_type("ve.test/beta");
    CHECK(alpha == 0u);
    CHECK(beta == 1u);
    CHECK(w.register_component_type("ve.test/alpha") == alpha);  // dup name
    REQUIRE(w.component_type_names().size() == 2);
    CHECK(w.component_type_names()[0] == "ve.test/alpha");  // names echo opaquely
    CHECK(w.component_type_names()[1] == "ve.test/beta");

    // --- creation order is the iteration order ---------------------------
    const auto e0 = w.create();
    const auto e1 = w.create();
    const auto e2 = w.create();
    CHECK(e0.slot == 0);
    CHECK(e1.slot == 1);
    CHECK(e2.slot == 2);
    CHECK(e0.generation == 0u);
    CHECK(w.entity_count() == 3);
    CHECK(w.alive(e0));
    CHECK(w.alive(e1));
    CHECK(w.alive(e2));

    // --- typed round-trip through the void* storage ----------------------
    w.emplace(e0, alpha, test_alpha{7});
    w.emplace(e1, alpha, test_alpha{});
    w.emplace(e1, beta, test_beta{2.5});
    CHECK(w.get<test_alpha>(e0, alpha) != nullptr);
    const int v7 = w.get<test_alpha>(e0, alpha)->v;
    CHECK(v7 == 7);
    CHECK_FALSE(w.has(e0, beta));
    CHECK(w.has(e1, alpha));
    CHECK(w.has(e1, beta));
    if (auto* p = w.get<test_beta>(e1, beta)) p->w = 4.0;  // reference mutates in place
    const double w4 = w.get<test_beta>(e1, beta)->w;
    CHECK(w4 == 4.0);
    // Wrong T through the wrong index is NOT prevented here (dispatch is by
    // index; the CALLER pairs T with the index it registered) — pin that the
    // right pair round-trips; replace swaps the blob cleanly.
    w.emplace(e0, alpha, test_alpha{9});
    const int v9 = w.get<test_alpha>(e0, alpha)->v;
    CHECK(v9 == 9);

    // --- tree view: slot order, types ascending by registration index ----
    auto tree = w.entities();
    REQUIRE(tree.size() == 3);
    CHECK(tree[0].id == e0);
    CHECK(tree[0].types == std::vector<std::uint32_t>{alpha});
    CHECK(tree[1].types == std::vector<std::uint32_t>{alpha, beta});
    CHECK(tree[2].types.empty());

    // --- remove ----------------------------------------------------------
    CHECK(w.remove(e0, alpha));
    CHECK_FALSE(w.remove(e0, alpha));  // double-remove is a no, not a crash
    CHECK_FALSE(w.has(e0, alpha));

    // --- destroy + generation guard: stale ids must NEVER alias the
    //     reused slot (mutation-checked guard: alive()'s generation match)
    CHECK(w.destroy(e1));
    CHECK_FALSE(w.destroy(e1));  // already dead
    CHECK_FALSE(w.alive(e1));
    CHECK(w.entity_count() == 2);
    const auto e1b = w.create();  // LIFO free list reuses slot 1
    CHECK(e1b.slot == 1);
    CHECK(e1b.generation == 1u);
    CHECK_FALSE(w.alive(e1));  // the STALE id is provably dead
    CHECK_FALSE(w.has(e1, alpha));
    CHECK(w.get<test_alpha>(e1, beta) == nullptr);
    // Components die with the entity: reused slot starts empty.
    CHECK(w.entities()[1].types.empty());

    // --- register-after-create grows existing slots' columns -------------
    const auto gamma = w.register_component_type("ve.test/gamma");
    CHECK(gamma == 2u);
    CHECK_FALSE(w.has(e0, gamma));
    w.emplace(e0, gamma, test_beta{1.0});
    const double wg = w.get<test_beta>(e0, gamma)->w;
    CHECK(wg == 1.0);

    // --- determinism: two identical worlds produce identical trees -------
    ve::core::ecs_world a, b;
    auto build = [&](ve::core::ecs_world& x) {
        const auto t0 = x.register_component_type("ve.test/alpha");
        const auto t1 = x.register_component_type("ve.test/beta");
        const auto x0 = x.create();
        const auto x1 = x.create();
        x.emplace(x1, t0, test_alpha{1});
        x.emplace(x0, t1, test_beta{2});
        x.destroy(x1);
        x.create();
    };
    build(a);
    build(b);
    const auto ta = a.entities();
    const auto tb = b.entities();
    CHECK(ta == tb);
}
