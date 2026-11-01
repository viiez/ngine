#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

import ngine.core;
import ngine.reflect;
import ngine.serialize;

using namespace ngine;

namespace {

struct Vec3 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    bool operator==(const Vec3&) const = default;
};

struct Item {
    std::string name;
    Vec3 position;
    i8 small = 0;
    u16 count = 0;
    i64 big = 0;
    f64 weight = 0.0;
    bool broken = false;
    bool operator==(const Item&) const = default;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    types.add<Vec3>("Vec3").field("x", &Vec3::x).field("y", &Vec3::y).field("z", &Vec3::z);
    types.add<Item>("Item")
        .field("name", &Item::name)
        .field("position", &Item::position)
        .field("small", &Item::small)
        .field("count", &Item::count)
        .field("big", &Item::big)
        .field("weight", &Item::weight)
        .field("broken", &Item::broken);
    return types;
}

Item sample() {
    return Item{
        .name = "Laterne",
        .position = {1.5f, -2.25f, 1e-7f},
        .small = -100,
        .count = 65535,
        .big = -9'000'000'000'000,
        .weight = 0.1,
        .broken = true,
    };
}

} // namespace

TEST_CASE("Reflected structs round trip through JSON", "[serialize][reflect]") {
    const auto types = make_types();
    const Item item = sample();

    const json::Value value = serialize::to_json(item, types);
    CHECK(*value.find("name")->string() == "Laterne");
    CHECK(value.find("position")->find("y")->to_f64() == -2.25);

    const auto text = json::write(value);
    const auto parsed = json::parse(text);
    REQUIRE(parsed);
    Item read;
    REQUIRE(serialize::from_json(*parsed, read, types));
    CHECK(read == item);
}

TEST_CASE("Missing JSON fields keep defaults and unknown fields are skipped",
          "[serialize][reflect]") {
    const auto types = make_types();
    const auto value = json::parse(R"({"count": 7, "neu": "egal", "position": {"z": 4}})");
    REQUIRE(value);
    Item item;
    item.name = "bleibt";
    REQUIRE(serialize::from_json(*value, item, types));
    CHECK(item.name == "bleibt");
    CHECK(item.count == 7);
    CHECK(item.position == Vec3{0.0f, 0.0f, 4.0f});
}

TEST_CASE("JSON values that do not fit report the field path", "[serialize][reflect]") {
    const auto types = make_types();
    Item item;

    const auto too_big = serialize::from_json(*json::parse(R"({"count": 70000})"), item, types);
    REQUIRE_FALSE(too_big);
    CHECK(too_big.error().find("count") != std::string::npos);

    const auto wrong_type =
        serialize::from_json(*json::parse(R"({"position": {"x": "eins"}})"), item, types);
    REQUIRE_FALSE(wrong_type);
    CHECK(wrong_type.error().find("position.x") != std::string::npos);

    CHECK_FALSE(serialize::from_json(*json::parse(R"({"small": -129})"), item, types));
    CHECK_FALSE(serialize::from_json(*json::parse(R"({"broken": 1})"), item, types));
    CHECK_FALSE(serialize::from_json(*json::parse("[]"), item, types));
}

TEST_CASE("Reflected structs round trip through binary", "[serialize][reflect]") {
    const auto types = make_types();
    const Item item = sample();

    serialize::BinaryWriter writer;
    serialize::write_binary(writer, item, types);
    // 4 + 7 (Text) + 12 (Vec3) + 1 + 2 + 8 + 8 + 1 Bytes.
    CHECK(writer.bytes().size() == 43);

    serialize::BinaryReader reader{writer.bytes()};
    Item read;
    REQUIRE(serialize::read_binary(reader, read, types));
    CHECK(read == item);
    CHECK(reader.remaining() == 0);
}

TEST_CASE("Binary format is little endian", "[serialize]") {
    serialize::BinaryWriter writer;
    writer.write(u32{0x11223344});
    const auto bytes = writer.bytes();
    REQUIRE(bytes.size() == 4);
    CHECK(bytes[0] == std::byte{0x44});
    CHECK(bytes[3] == std::byte{0x11});
}

TEST_CASE("Truncated binary data is rejected", "[serialize][reflect]") {
    const auto types = make_types();
    serialize::BinaryWriter writer;
    serialize::write_binary(writer, sample(), types);
    const auto bytes = writer.bytes();

    for (usize size = 0; size < bytes.size(); ++size) {
        serialize::BinaryReader reader{bytes.first(size)};
        Item read;
        CHECK_FALSE(serialize::read_binary(reader, read, types));
    }
}

TEST_CASE("Binary strings with a corrupt length do not allocate", "[serialize]") {
    serialize::BinaryWriter writer;
    writer.write(u32{0xFFFFFFFF});
    serialize::BinaryReader reader{writer.bytes()};
    std::string text;
    CHECK_FALSE(reader.read_string(text));
    CHECK(reader.position() == 0);
}

TEST_CASE("f32 fields are written in their shortest form", "[serialize][reflect]") {
    const auto types = make_types();
    Vec3 v{0.1f, 1e-7f, 3.4028235e38f};
    const json::Value value = serialize::to_json(v, types);
    CHECK(json::write(value, {.pretty = false}) == R"({"x":0.1,"y":1e-07,"z":3.4028235e+38})");
    Vec3 read;
    REQUIRE(serialize::from_json(value, read, types));
    CHECK(read == v);
}
