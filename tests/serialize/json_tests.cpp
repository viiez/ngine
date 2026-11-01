#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>

import ngine.core;
import ngine.serialize;

using namespace ngine;

TEST_CASE("JSON parses all value types", "[serialize][json]") {
    const auto value = json::parse(R"({
        "null": null, "wahr": true, "falsch": false,
        "ganz": -42, "gross": 18446744073709551615, "komma": 2.5e-3,
        "text": "Gr\u00f6\u00dfe \"zitiert\"\n", "liste": [1, [2], {}]
    })");
    REQUIRE(value);
    CHECK(value->find("null")->is_null());
    CHECK(value->find("wahr")->to_bool() == true);
    CHECK(value->find("falsch")->to_bool() == false);
    CHECK(value->find("ganz")->to_i64() == -42);
    CHECK(value->find("gross")->type() == json::Type::Unsigned);
    CHECK(value->find("gross")->to_u64() == std::numeric_limits<u64>::max());
    CHECK(value->find("komma")->to_f64() == 2.5e-3);
    CHECK(*value->find("text")->string() == "Gr\xC3\xB6\xC3\x9F"
                                            "e \"zitiert\"\n");
    REQUIRE(value->find("liste")->array() != nullptr);
    CHECK(value->find("liste")->array()->size() == 3);
}

TEST_CASE("JSON keeps the order of object keys", "[serialize][json]") {
    const auto value = json::parse(R"({"z": 1, "a": 2, "m": 3})");
    REQUIRE(value);
    const json::Object& members = *value->object();
    REQUIRE(members.size() == 3);
    CHECK(members[0].key == "z");
    CHECK(members[1].key == "a");
    CHECK(members[2].key == "m");
}

TEST_CASE("JSON surrogate pairs become UTF-8", "[serialize][json]") {
    const auto value = json::parse(R"("\ud83d\ude00")");
    REQUIRE(value);
    CHECK(*value->string() == "\xF0\x9F\x98\x80");
}

TEST_CASE("JSON rejects invalid input with position", "[serialize][json]") {
    CHECK_FALSE(json::parse(""));
    CHECK_FALSE(json::parse("{"));
    CHECK_FALSE(json::parse("[1, 2,]"));
    CHECK_FALSE(json::parse("{\"a\" 1}"));
    CHECK_FALSE(json::parse("01"));
    CHECK_FALSE(json::parse("1."));
    CHECK_FALSE(json::parse("\"offen"));
    CHECK_FALSE(json::parse("\"\\x\""));
    CHECK_FALSE(json::parse("tru"));
    CHECK_FALSE(json::parse("1 2"));
    CHECK_FALSE(json::parse("\"\\ud800\""));

    const auto error = json::parse("{\n  \"a\": ,\n}");
    REQUIRE_FALSE(error);
    CHECK(error.error().line == 2);
    CHECK(error.error().column == 8);
}

TEST_CASE("JSON limits nesting depth", "[serialize][json]") {
    const std::string deep(1000, '[');
    CHECK_FALSE(json::parse(deep));
    const std::string ok = std::string(100, '[') + std::string(100, ']');
    CHECK(json::parse(ok));
}

TEST_CASE("JSON writes and reads back the same value", "[serialize][json]") {
    json::Value value;
    value["name"] = "Sonne \"hell\"\t\x01";
    value["radius"] = 696340.0;
    value["kleine Zahl"] = 0.1f;
    value["minus null"] = -0.0;
    value["ganz"] = -7;
    value["gross"] = std::numeric_limits<u64>::max();
    value["leer"] = json::Value{json::Array{}};
    value["liste"].push_back(true);
    value["liste"].push_back(nullptr);

    for (const bool pretty : {true, false}) {
        const std::string text = json::write(value, {.pretty = pretty});
        const auto read = json::parse(text);
        REQUIRE(read);
        CHECK(*read == value);
        CHECK(std::signbit(*read->find("minus null")->to_f64()));
    }
}

TEST_CASE("JSON floats round trip exactly", "[serialize][json]") {
    for (const f64 number : {0.1, 1.0 / 3.0, 1e300, -2.5e-308, 123456789.123456789}) {
        const auto read = json::parse(json::write(json::Value{number}));
        REQUIRE(read);
        CHECK(read->to_f64() == number);
    }
    const f32 small = 0.1f;
    const auto read = json::parse(json::write(json::Value{small}));
    CHECK(static_cast<f32>(*read->to_f64()) == small);
}

TEST_CASE("JSON writes NaN as null", "[serialize][json]") {
    CHECK(json::write(json::Value{std::numeric_limits<f64>::quiet_NaN()}, {.pretty = false}) ==
          "null");
}

TEST_CASE("JSON pretty output is indented", "[serialize][json]") {
    json::Value value;
    value["a"].push_back(1);
    CHECK(json::write(value) == "{\n  \"a\": [\n    1\n  ]\n}\n");
    CHECK(json::write(value, {.pretty = false}) == "{\"a\":[1]}");
}

TEST_CASE("JSON number conversions check the range", "[serialize][json]") {
    CHECK(json::Value{3.0}.to_i64() == 3);
    CHECK_FALSE(json::Value{3.5}.to_i64());
    CHECK_FALSE(json::Value{-1}.to_u64());
    CHECK_FALSE(json::Value{"1"}.to_f64());
    CHECK(json::Value{u64{5}}.type() == json::Type::Int);
}
