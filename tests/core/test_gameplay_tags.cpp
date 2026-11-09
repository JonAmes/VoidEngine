// Phase 6 task 6.2 — hierarchical gameplay tags: ancestor queries with
// dot-boundary strictness, insertion-order determinism.
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include "ve/core/sim/tags.h"

using ve::core::TagSet;

TEST_CASE("gameplay_tags") {
    SUBCASE("add / duplicate / empty") {
        TagSet s;
        CHECK(s.add("pilot.stunned"));
        CHECK_FALSE(s.add("pilot.stunned"));  // set semantics
        CHECK_FALSE(s.add(""));               // empty tag rejected
        CHECK(s.size() == 1);
        CHECK(s.add("faction.hostile"));
        CHECK(s.tags() ==
              std::vector<std::string>{"pilot.stunned", "faction.hostile"});
    }

    SUBCASE("ancestor queries answer for descendants") {
        TagSet s;
        s.add("pilot.stunned");
        s.add("ship.shields.down");
        CHECK(s.contains("pilot"));
        CHECK(s.contains("pilot.stunned"));
        CHECK(s.contains("ship.shields"));
        CHECK(s.contains("ship"));
        CHECK_FALSE(s.contains("stunned"));      // not a suffix match
        CHECK_FALSE(s.contains("pilot.unconscious"));
        CHECK_FALSE(s.contains(""));             // empty query answers nothing
    }

    SUBCASE("dot-boundary strictness — the load-bearing rule") {
        TagSet s;
        s.add("pilot.stunned");
        CHECK_FALSE(s.contains("pilot.stun"));     // half-typed tag must NOT match
        CHECK_FALSE(s.contains("pilot.stunn"));
        s.add("pilotstunned");                     // missing dot entirely
        TagSet u;
        u.add("pilotstunned");
        CHECK_FALSE(u.contains("pilot"));          // does not become an ancestor
        TagSet t;
        t.add("a");
        CHECK(t.contains("a"));
        CHECK_FALSE(t.contains("ab"));             // query longer than tag: no match
        CHECK_FALSE(t.contains(""));               // "" never matches anything
    }

    SUBCASE("contains_exact vs contains") {
        TagSet s;
        s.add("crew.ko.pilot");
        CHECK(s.contains("crew"));
        CHECK(s.contains_exact("crew.ko.pilot"));
        CHECK_FALSE(s.contains_exact("crew"));     // ancestor answers loosely only
    }

    SUBCASE("count_matching counts every answering tag") {
        TagSet s;
        s.add("crew.ko.pilot");
        s.add("crew.ko.engineer");
        s.add("ship.shields.down");
        CHECK(s.count_matching("crew") == 2);
        CHECK(s.count_matching("crew.ko") == 2);
        CHECK(s.count_matching("ship") == 1);
        CHECK(s.count_matching("nothing") == 0);
        CHECK(s.count_matching("") == 0);
    }

    SUBCASE("remove is exact; descendants survive independently") {
        TagSet s;
        s.add("faction.hostile");
        s.add("faction.hostile.alert");
        CHECK(s.remove("faction.hostile"));
        CHECK_FALSE(s.remove("faction.hostile"));
        CHECK_FALSE(s.contains_exact("faction.hostile"));
        CHECK(s.contains("faction"));              // descendant still answers
        CHECK(s.size() == 1);
        s.clear();
        CHECK(s.empty());
    }

    SUBCASE("deterministic iteration order across builds of the same ops") {
        // Same operation sequence -> byte-identical order, twice. Anything
        // later that iterates tags for sim effects relies on this.
        auto build = [] {
            TagSet s;
            s.add("zulu.tag");
            s.add("alpha.tag");
            s.add("mike.tag");
            s.remove("alpha.tag");
            s.add("alpha.tag");  // re-add appends at the end, like StatsSet
            return s;
        };
        const TagSet a = build();
        const TagSet b = build();
        CHECK(a.tags() == b.tags());
        CHECK(a.tags() == std::vector<std::string>{"zulu.tag", "mike.tag", "alpha.tag"});
    }
}
