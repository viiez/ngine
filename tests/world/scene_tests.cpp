#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

import ngine.core;
import ngine.reflect;
import ngine.filesystem;
import ngine.world;

using namespace ngine;

namespace {

struct Orbit {
    f64 radius = 0.0;
    f32 speed = 0.0f;
    f32 angle = 0.0f;
};

struct Planet {
    std::string kind;
    bool habitable = false;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<Orbit>("Orbit")
        .field("radius", &Orbit::radius)
        .field("speed", &Orbit::speed)
        .field("angle", &Orbit::angle);
    types.add<Planet>("Planet").field("kind", &Planet::kind).field("habitable", &Planet::habitable);
    return types;
}

void build_solar_system(world::World& world) {
    world::Entity sun = world.create("Sonne");
    world::Entity earth = world.create("Erde");
    earth.set(Orbit{1.496e11, 0.01f, 0.5f}).set(Planet{"Gestein", true}).set_parent(sun);
    world::Entity moon = world.create("Mond");
    moon.set(Orbit{3.844e8, 0.1f, 1.0f}).set_parent(earth);
    world::Entity mars = world.create("Mars");
    mars.set(Orbit{2.279e11, 0.008f, 2.0f}).set(Planet{"Gestein", false}).set_parent(sun);
    world.create("Komet ohne Eltern").set(Orbit{1e12, 0.001f, 0.0f});
}

} // namespace

TEST_CASE("Scenes round trip with hierarchy and components", "[world][scene]") {
    const auto types = make_types();
    world::World original{types};
    build_solar_system(original);
    const std::string text = world::save_scene(original);

    world::World loaded{types};
    const auto count = world::load_scene(loaded, text);
    REQUIRE(count);
    CHECK(*count == 5);
    CHECK(loaded.entity_count() == 5);
    // Dieselbe Szene ergibt denselben Text: Hierarchie, Reihenfolge und Werte sind gleich.
    CHECK(world::save_scene(loaded) == text);

    int moons = 0;
    loaded.each<const world::Name>([&](world::Entity entity, const world::Name& name) {
        if (name.value == "Mond") {
            ++moons;
            REQUIRE(entity.parent());
            CHECK(entity.parent().name() == "Erde");
            CHECK(entity.parent().parent().name() == "Sonne");
            CHECK(entity.get<Orbit>()->radius == 3.844e8);
        }
    });
    CHECK(moons == 1);
}

TEST_CASE("Scene files are written and read", "[world][scene]") {
    const auto types = make_types();
    world::World original{types};
    build_solar_system(original);

    const auto base = std::filesystem::temp_directory_path() / "ngine_scene_tests";
    const std::u8string base_text = base.u8string();
    const std::string dir{base_text.begin(), base_text.end()};
    const std::string path = filesystem::join(dir, "sonnensystem.scene.json");
    REQUIRE(world::save_scene_file(original, path));

    world::World loaded{types};
    const auto count = world::load_scene_file(loaded, path);
    REQUIRE(count);
    CHECK(*count == 5);
    static_cast<void>(filesystem::remove(dir));
}

TEST_CASE("Scenes skip unknown components and keep defaults for missing fields", "[world][scene]") {
    const auto types = make_types();
    world::World world{types};
    const auto count = world::load_scene(world, R"({
        "format": "ngine.scene", "version": 1,
        "entities": [
            {"id": 7, "components": {"Name": {"value": "A"}, "Gibt es nicht": {}}},
            {"id": 3, "parent": 7, "components": {"Orbit": {"speed": 2}}}
        ]
    })");
    REQUIRE(count);
    CHECK(*count == 2);
    world.each<const Orbit>([](world::Entity entity, const Orbit& orbit) {
        CHECK(orbit.speed == 2.0f);
        CHECK(orbit.radius == 0.0);
        CHECK(entity.parent().name() == "A");
    });
}

TEST_CASE("Scenes keep their order when the world reuses ids", "[world][scene]") {
    const auto types = make_types();
    world::World original{types};
    build_solar_system(original);
    const std::string text = world::save_scene(original);

    // Gelöschte Entities hinterlassen Kennungen, die die Welt in umgekehrter Reihenfolge wieder
    // vergibt (wie beim Öffnen einer Szene im Editor).
    world::World loaded{types};
    std::vector<world::Entity> old;
    for (int i = 0; i < 8; ++i) {
        old.push_back(loaded.create());
    }
    for (world::Entity& entity : old) {
        entity.destroy();
    }
    REQUIRE(world::load_scene(loaded, text));
    CHECK(world::save_scene(loaded) == text);
}

TEST_CASE("Failed scene loads leave no empty entities", "[world][scene]") {
    const auto types = make_types();
    world::World world{types};
    CHECK_FALSE(world::load_scene(world, R"({"format": "ngine.scene", "version": 1,
        "entities": [{"id": 1}, {"id": 2, "parent": 7}, {"id": 3}]})"));
    // Die erste Entity ist geladen; die zweite scheitert und bleibt wie die dritte nicht übrig
    // (seit R2.5 prüft das Laden den Elternteil, bevor es die Entity vergibt).
    CHECK(world.entity_count() == 1);
}

TEST_CASE("Invalid scenes report errors", "[world][scene]") {
    const auto types = make_types();
    world::World world{types};
    CHECK_FALSE(world::load_scene(world, "kein json"));
    CHECK_FALSE(world::load_scene(world, R"({"format": "anders", "version": 1, "entities": []})"));
    CHECK_FALSE(
        world::load_scene(world, R"({"format": "ngine.scene", "version": 2, "entities": []})"));
    CHECK_FALSE(world::load_scene(
        world, R"({"format": "ngine.scene", "version": 1, "entities": [{"id": 1, "parent": 2}]})"));
    CHECK_FALSE(world::load_scene(
        world, R"({"format": "ngine.scene", "version": 1, "entities": [{"id": 1}, {"id": 1}]})"));

    const auto wrong_value = world::load_scene(world, R"({
        "format": "ngine.scene", "version": 1,
        "entities": [{"id": 1, "components": {"Orbit": {"radius": "weit"}}}]
    })");
    REQUIRE_FALSE(wrong_value);
    CHECK(wrong_value.error().find("radius") != std::string::npos);
}

TEST_CASE("An empty world saves an empty scene", "[world][scene]") {
    const auto types = make_types();
    const world::World world{types};
    const std::string text = world::save_scene(world);
    world::World loaded{types};
    CHECK(world::load_scene(loaded, text) == 0u);
}
