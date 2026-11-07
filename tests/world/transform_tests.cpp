#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.reflect;
import ngine.world;

using namespace ngine;
using math::DVec3;
using math::Quat;
using math::Vec3;

namespace {

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    return types;
}

world::Transform at(DVec3 position, Quat rotation = math::quat_identity(),
                    Vec3 scale = Vec3{1, 1, 1}) {
    return world::Transform{.position = position, .rotation = rotation, .scale = scale};
}

DVec3 world_position(const world::Entity& entity) {
    return entity.get<world::WorldTransform>()->position();
}

} // namespace

TEST_CASE("Transform brings WorldTransform along", "[world][transform]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity entity = world.create().set(at(DVec3{1.0, 2.0, 3.0}));
    REQUIRE(entity.has<world::WorldTransform>());

    world::update_transforms(world);
    CHECK(math::near(world_position(entity), DVec3{1.0, 2.0, 3.0}, 0.0));
}

TEST_CASE("Transforms propagate from parents to children", "[world][transform]") {
    const auto types = make_types();
    world::World world{types};

    // Kinder zuerst anlegen: Die Reihenfolge der Berechnung folgt der Hierarchie, nicht dem
    // Alter der Entities.
    world::Entity grandchild = world.create("Enkel").set(at(DVec3{0.0, 0.0, -1.0}));
    world::Entity child = world.create("Kind").set(at(DVec3{1.0, 0.0, 0.0}));
    world::Entity root = world.create("Wurzel").set(
        at(DVec3{100.0, 0.0, 0.0}, math::angle_axis(math::radians(90.0f), Vec3{0, 1, 0}),
           Vec3{2, 2, 2}));
    child.set_parent(root);
    grandchild.set_parent(child);

    world::update_transforms(world);
    // Kind: (1, 0, 0) skaliert auf 2, um y gedreht nach (0, 0, -2), verschoben um 100.
    CHECK(math::near(world_position(child), DVec3{100.0, 0.0, -2.0}, 1e-6));
    // Enkel: (0, 0, -1) skaliert auf -2, gedreht nach (-2, 0, 0), dazu die Lage des Kindes.
    CHECK(math::near(world_position(grandchild), DVec3{98.0, 0.0, -2.0}, 1e-6));

    // Bewegt sich die Wurzel, ziehen die Kinder im nächsten Durchlauf mit.
    root.get_mut<world::Transform>()->position = DVec3{0, 0, 0};
    root.modified<world::Transform>();
    world::update_transforms(world);
    CHECK(math::near(world_position(grandchild), DVec3{-2.0, 0.0, -2.0}, 1e-6));
}

TEST_CASE("Ancestors without Transform are skipped", "[world][transform]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity root = world.create().set(at(DVec3{10.0, 0.0, 0.0}));
    world::Entity group = world.create("Gruppe");
    world::Entity leaf = world.create().set(at(DVec3{0.0, 1.0, 0.0}));
    group.set_parent(root);
    leaf.set_parent(group);

    world::update_transforms(world);
    CHECK(math::near(world_position(leaf), DVec3{10.0, 1.0, 0.0}, 1e-9));
}

TEST_CASE("Deep hierarchies keep 64 bit precision far from the origin", "[world][transform]") {
    const auto types = make_types();
    world::World world{types};
    const DVec3 far{1.0e7, 0.0, -1.0e7};
    world::Entity parent = world.create().set(at(far));
    for (int i = 0; i < 10; ++i) {
        world::Entity child = world.create().set(at(DVec3{0.001, 0.0, 0.0}));
        child.set_parent(parent);
        parent = child;
    }
    world::update_transforms(world);
    // Zehn Additionen bei 1e7 runden in 64 Bit jeweils um höchstens ein paar Nanometer.
    CHECK(math::near(world_position(parent), far + DVec3{0.01, 0.0, 0.0}, 1e-7));
}

TEST_CASE("Scenes save Transform but not WorldTransform", "[world][transform][scene]") {
    const auto types = make_types();
    std::string text;
    {
        world::World world{types};
        world::Entity root = world.create("Wurzel").set(at(DVec3{1.0e7, 0.5, 0.25}));
        world::Entity child = world.create("Kind").set(at(DVec3{0.0, 1.0, 0.0}));
        child.set_parent(root);
        world::update_transforms(world);
        text = world::save_scene(world);
    }
    CHECK(text.find("\"Transform\"") != std::string::npos);
    CHECK(text.find("WorldTransform") == std::string::npos);

    world::World loaded{types};
    REQUIRE(world::load_scene(loaded, text).has_value());
    world::update_transforms(loaded);
    usize checked = 0;
    loaded.each<const world::Name, const world::WorldTransform>(
        [&](const world::Name& name, const world::WorldTransform& transform) {
            if (name.value == "Kind") {
                CHECK(math::near(transform.position(), DVec3{1.0e7, 1.5, 0.25}, 0.0));
                ++checked;
            }
        });
    CHECK(checked == 1);
}

// --- Nur geänderte Teilbäume (TD-15) ------------------------------------------------------------

TEST_CASE("update_transforms recomputes only changed subtrees", "[world][transform]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity moving = world.create("Bewegt").set(at(DVec3{1.0, 0.0, 0.0}));
    // Ohne Name liegt sie in einer anderen Tabelle als "Bewegt": Die Erkennung arbeitet pro
    // Tabelle.
    world::Entity still = world.create().set(at(DVec3{0.0, 5.0, 0.0}));
    std::vector<world::Entity> children;
    for (int i = 0; i < 3; ++i) {
        world::Entity child = world.create().set(at(DVec3{0.0, 0.0, static_cast<f64>(i)}));
        child.set_parent(moving);
        children.push_back(child);
    }
    world::Entity other_child = world.create().set(at(DVec3{0.0, 1.0, 0.0}));
    other_child.set_parent(still);

    CHECK(world::update_transforms(world) == 6);
    // Nichts geändert: nichts neu berechnet.
    CHECK(world::update_transforms(world) == 0);

    // Wurzel bewegt: sie und ihre Kinder, der andere Teilbaum bleibt.
    moving.set(at(DVec3{2.0, 0.0, 0.0}));
    const usize updated = world::update_transforms(world);
    CHECK(updated >= 4);
    CHECK(updated < 6);
    CHECK(math::near(world_position(children[2]), DVec3{2.0, 0.0, 2.0}, 1e-9));
    CHECK(math::near(world_position(other_child), DVec3{0.0, 6.0, 0.0}, 1e-9));

    // Ein Kind über get_mut() und modified() geändert: nur sein Chunk.
    children[0].get_mut<world::Transform>()->position = DVec3{0.0, 3.0, 0.0};
    children[0].modified<world::Transform>();
    CHECK(world::update_transforms(world) == 3);
    CHECK(math::near(world_position(children[0]), DVec3{2.0, 3.0, 0.0}, 1e-9));
    CHECK(world::update_transforms(world) == 0);
}

TEST_CASE("update_transforms follows reparenting and new entities", "[world][transform]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity a = world.create().set(at(DVec3{10.0, 0.0, 0.0}));
    world::Entity b = world.create().set(at(DVec3{0.0, 0.0, 20.0}));
    world::Entity leaf = world.create().set(at(DVec3{0.0, 1.0, 0.0}));
    leaf.set_parent(a);
    (void)world::update_transforms(world);
    CHECK(math::near(world_position(leaf), DVec3{10.0, 1.0, 0.0}, 1e-9));

    leaf.set_parent(b);
    CHECK(world::update_transforms(world) >= 1);
    CHECK(math::near(world_position(leaf), DVec3{0.0, 1.0, 20.0}, 1e-9));

    world::Entity grandchild = world.create().set(at(DVec3{0.0, 0.0, 1.0}));
    grandchild.set_parent(leaf);
    CHECK(world::update_transforms(world) >= 1);
    CHECK(math::near(world_position(grandchild), DVec3{0.0, 1.0, 21.0}, 1e-9));

    // Großeltern bewegt: auch der Enkel zieht mit, obwohl sich an seiner Tabelle nichts ändert.
    b.set(at(DVec3{0.0, 0.0, 30.0}));
    (void)world::update_transforms(world);
    CHECK(math::near(world_position(grandchild), DVec3{0.0, 1.0, 31.0}, 1e-9));
}

TEST_CASE("update_transforms sees Transform written by systems", "[world][transform]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity parent = world.create().set(at(DVec3{0.0, 0.0, 0.0}));
    world::Entity child = world.create().set(at(DVec3{1.0, 0.0, 0.0}));
    child.set_parent(parent);
    (void)world::update_transforms(world);

    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Schedule schedule{world};
    schedule.add<world::Transform>("Steigen", [](world::Entity entity, world::Transform& t) {
        if (entity.parent() == world::Entity{}) {
            t.position.y += 1.0;
        }
    });
    schedule.run(jobs);
    (void)world::update_transforms(world);
    CHECK(math::near(world_position(child), DVec3{1.0, 1.0, 0.0}, 1e-9));
}

TEST_CASE("world_affine follows the hierarchy without WorldTransform", "[world][transform]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity root = world.create().set(
        at(DVec3{100.0, 0.0, 0.0}, math::angle_axis(math::radians(90.0f), Vec3{0, 1, 0}),
           Vec3{2, 2, 2}));
    world::Entity group = world.create("Gruppe");
    world::Entity leaf = world.create().set(at(DVec3{1.0, 0.0, 0.0}));
    group.set_parent(root);
    leaf.set_parent(group);
    // Ohne update_transforms(): direkt aus den Transforms.
    CHECK(math::near(world::world_affine(world, leaf.id()).translation, DVec3{100.0, 0.0, -2.0},
                     1e-6));
    (void)world::update_transforms(world);
    CHECK(math::near(world::world_affine(world, leaf.id()),
                     leaf.get<world::WorldTransform>()->value, 1e-6f, 1e-9));
}
