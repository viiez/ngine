#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

import ngine.core;
import ngine.reflect;

using namespace ngine;

namespace {

struct Vec3 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
};

struct Body {
    std::string label = "Körper";
    Vec3 position;
    u8 layer = 3;
    bool active = true;
};

} // namespace

TEST_CASE("Primitive types are registered from the start", "[reflect]") {
    const reflect::TypeRegistry types;
    REQUIRE(types.find<f32>() != nullptr);
    CHECK(types.find<f32>()->kind == reflect::Kind::Float);
    CHECK(types.find<i16>()->kind == reflect::Kind::Int);
    CHECK(types.find<u64>()->kind == reflect::Kind::UInt);
    CHECK(types.find<bool>()->kind == reflect::Kind::Bool);
    CHECK(types.find<std::string>()->kind == reflect::Kind::String);
    CHECK(types.find("f64") == types.find<f64>());
}

TEST_CASE("Structs describe their fields with offsets", "[reflect]") {
    reflect::TypeRegistry types;
    types.add<Vec3>("Vec3").field("x", &Vec3::x).field("y", &Vec3::y).field("z", &Vec3::z);
    types.add<Body>("Body")
        .field("label", &Body::label)
        .field("position", &Body::position)
        .field("layer", &Body::layer)
        .field("active", &Body::active);

    const reflect::TypeInfo& body = types.get<Body>();
    CHECK(body.name == "Body");
    CHECK(body.size == sizeof(Body));
    CHECK(body.alignment == alignof(Body));
    CHECK(body.kind == reflect::Kind::Struct);
    CHECK_FALSE(body.trivial);
    REQUIRE(body.fields.size() == 4);

    // offsetof ist für Typen mit std::string nicht garantiert; deshalb über Adressen.
    const Body sample;
    const auto offset = [&](const auto& member) {
        return static_cast<usize>(reinterpret_cast<const std::byte*>(&member) -
                                  reinterpret_cast<const std::byte*>(&sample));
    };
    const reflect::Field* position = body.find_field("position");
    REQUIRE(position != nullptr);
    CHECK(position->type == types.find<Vec3>());
    CHECK(position->offset == offset(sample.position));
    CHECK(body.find_field("layer")->offset == offset(sample.layer));
    CHECK(body.find_field("fehlt") == nullptr);
    CHECK(types.get<Vec3>().trivial);
}

TEST_CASE("Type ids are stable hashes of the name", "[reflect]") {
    reflect::TypeRegistry types;
    types.add<Vec3>("Vec3");
    // FNV-1a ist festgelegt; der Wert darf sich nie ändern, sonst passen gespeicherte Daten nicht.
    static_assert(reflect::type_id("") == 14695981039346656037ull);
    CHECK(types.get<Vec3>().id == reflect::type_id("Vec3"));
    CHECK(types.find_id(reflect::type_id("Vec3")) == types.find<Vec3>());
    CHECK(types.find("Vec4") == nullptr);
}

TEST_CASE("Type operations construct, copy, move and destroy", "[reflect]") {
    reflect::TypeRegistry types;
    types.add<Vec3>("Vec3");
    types.add<Body>("Body").field("label", &Body::label);
    const reflect::TypeInfo& body = types.get<Body>();

    alignas(Body) std::byte a[sizeof(Body)];
    alignas(Body) std::byte b[sizeof(Body)];
    body.ops.construct(a);
    CHECK(reinterpret_cast<Body*>(a)->label == "Körper");

    reinterpret_cast<Body*>(a)->label = "ein langer Text, der nicht in den kleinen Puffer passt";
    body.ops.copy_construct(b, a);
    CHECK(reinterpret_cast<Body*>(b)->label == reinterpret_cast<Body*>(a)->label);

    body.ops.destroy(b);
    body.ops.move_construct(b, a);
    CHECK(reinterpret_cast<Body*>(b)->label.starts_with("ein langer"));

    body.ops.destroy(a);
    body.ops.destroy(b);
}

TEST_CASE("Registered types are listed in order", "[reflect]") {
    reflect::TypeRegistry types;
    const auto primitives = types.types().size();
    types.add<Vec3>("Vec3");
    REQUIRE(types.types().size() == primitives + 1);
    CHECK(types.types().back()->name == "Vec3");
}
