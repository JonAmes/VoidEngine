// Phase 5 task 5.7 — TOML subset parser + #42 unit coercion (see
// tools/cook/include/ve/cook/toml.h for the subset contract).
#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include <ve/cook/toml.h>

using namespace ve::cook;

TEST_CASE("cook_toml") {
    SUBCASE("full subset parses, tables iterate in key order") {
        const std::string doc = R"(
# hull def sample — comments and blanks ignored
class_tag = "frigate"          # trailing comment
mass = "1200 t"   # kept as string; coercion is per-field at 5.8
grid.cell = 2.5                  # dotted key -> nested tables
open = false

[sockets.engine_L]               # [a.b] header
cat = "main"
pos = [1.4, -4.5, 0.0]           # inline array of reals

[[mounts]]                       # array-of-tables (append)
comp = "thruster.main"
sock = "engine_L"
[[mounts]]
comp = "thruster.main"
sock = "engine_R"

[strings]
name = "Keel \u2014 \"slipknot\"\n"   # escapes incl. \uXXXX -> UTF-8
)";
        TomlValue root;
        std::string err;
        REQUIRE_MESSAGE(parse_toml(doc, root, &err), err);
        CHECK(root.kind == TomlValue::kTable);
        const auto& t = root.table;

        CHECK(t.at("class_tag").str == "frigate");
        CHECK(t.at("mass").is_string());
        CHECK(t.at("open").boolean == false);
        REQUIRE(t.count("grid"));
        CHECK(t.at("grid").table.at("cell").num() == doctest::Approx(2.5));

        const auto& eng = t.at("sockets").table.at("engine_L").table;
        CHECK(eng.at("cat").str == "main");
        REQUIRE(eng.at("pos").array.size() == 3);
        CHECK(eng.at("pos").array[0].num() == doctest::Approx(1.4));

        REQUIRE(t.at("mounts").kind == TomlValue::kArray);
        REQUIRE(t.at("mounts").array.size() == 2);
        CHECK(t.at("mounts").array[1].table.at("sock").str == "engine_R");

        // UTF-8 for em dash (U+2014) + escaped quote + newline
        const auto& name = t.at("strings").table.at("name").str;
        CHECK(name.find("\xE2\x80\x94") != std::string::npos);
        CHECK(name.find("\"") != std::string::npos);
        CHECK(name.back() == '\n');

        // determinism contract: key order, not insertion order
        std::vector<std::string> keys;
        for (const auto& [k, v] : t) keys.push_back(k);
        CHECK(keys == std::vector<std::string>{"class_tag", "grid", "mass", "mounts",
                                              "open", "sockets", "strings"});
    }

    SUBCASE("number grammar: ints signed, floats with fraction/exponent") {
        TomlValue root;
        std::string err;
        REQUIRE(parse_toml("a = -12\nb = 0.5\nc = 1e3\nd = -2.5E-2\ne = +7\n", root, &err));
        CHECK(root.table.at("a").kind == TomlValue::kInteger);
        CHECK(root.table.at("a").integer == -12);
        CHECK(root.table.at("b").kind == TomlValue::kReal);
        CHECK(root.table.at("c").num() == doctest::Approx(1000.0));
        CHECK(root.table.at("d").num() == doctest::Approx(-0.025));
        CHECK(root.table.at("e").integer == 7);
    }

    SUBCASE("arrays nest, trailing comma ok, empty array ok") {
        TomlValue root;
        std::string err;
        REQUIRE(parse_toml("m = [[1, 2], [3], ]\ne = []\ns = [\"a\", \"b\",]\n", root, &err));
        CHECK(root.table.at("m").array.size() == 2);
        CHECK(root.table.at("m").array[0].array.size() == 2);
        CHECK(root.table.at("e").array.empty());
        CHECK(root.table.at("s").array[1].str == "b");
    }

    SUBCASE("hard errors carry line numbers") {
        TomlValue root;
        std::string err;
        CHECK_FALSE(parse_toml("a = \"oops\n", root, &err));
        CHECK(err.find("line 1") != std::string::npos);
        CHECK(err.find("unterminated string") != std::string::npos);

        CHECK_FALSE(parse_toml("x = 1\ny = 2\nx = 3\n", root, &err));
        CHECK(err.find("line 3") != std::string::npos);
        CHECK(err.find("duplicate key") != std::string::npos);

        CHECK_FALSE(parse_toml("a = 1 2\n", root, &err));
        CHECK(err.find("garbage after value") != std::string::npos);

        CHECK_FALSE(parse_toml("a = 1e\n", root, &err));
        CHECK(err.find("bad exponent") != std::string::npos);

        CHECK_FALSE(parse_toml("a = \"\\q\"\n", root, &err));
        CHECK(err.find("unknown escape") != std::string::npos);

        CHECK_FALSE(parse_toml("a = 1\n[a]\n", root, &err));
        CHECK(err.find("cannot redefine") != std::string::npos);

        CHECK_FALSE(parse_toml("a = [1, 2\n", root, &err));
        CHECK(err.find("unterminated array") != std::string::npos);
    }

    SUBCASE("si_from_string: #42 coercion at parse time") {
        CHECK(*si_from_string("5 km") == doctest::Approx(5000.0));
        CHECK(*si_from_string("12 m") == doctest::Approx(12.0));
        CHECK(*si_from_string("2 day") == doctest::Approx(172800.0));
        CHECK(*si_from_string(" 1.5 AU ") == doctest::Approx(1.5 * 149597870700.0));
        CHECK(*si_from_string("-30 deg") == doctest::Approx(-30.0 * 3.14159265358979323846 / 180.0).epsilon(1e-12));
        CHECK(*si_from_string("0.5 deg/day") == doctest::Approx(0.5 * 3.14159265358979323846 / 180.0 / 86400.0).epsilon(1e-12));

        // (rejections pinned in the next subcase)
    }
    SUBCASE("si_from_string: hard rejections") {
        auto bare = si_from_string("500");
        REQUIRE_FALSE(bare.has_value());
        CHECK(bare.error().find("bare number") != std::string::npos);   // #42: suffix mandatory

        auto unk = si_from_string("5 furlongs");
        REQUIRE_FALSE(unk.has_value());
        CHECK(unk.error().find("unknown suffix") != std::string::npos);

        auto badnum = si_from_string("abc m");
        REQUIRE_FALSE(badnum.has_value());
        CHECK(badnum.error().find("bad number") != std::string::npos);

        auto inf = si_from_string("inf m");
        REQUIRE_FALSE(inf.has_value());
        CHECK(inf.error().find("bad number") != std::string::npos);  // from_chars rejects

        auto three = si_from_string("1 2 m");
        REQUIRE_FALSE(three.has_value());
        CHECK(three.error().find("too many tokens") != std::string::npos);
    }
}
