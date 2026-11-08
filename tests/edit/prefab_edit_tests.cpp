#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

import ngine.core;
import ngine.filesystem;
import ngine.math;
import ngine.reflect;
import ngine.world;
import ngine.edit;

using namespace ngine;

namespace {

struct Glow {
    f32 intensity = 1.0f;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<Glow>("Glow").field("intensity", &Glow::intensity);
    return types;
}

struct Fixture {
    reflect::TypeRegistry types = make_types();
    world::World world{types};
    events::EventBus bus;
    edit::SceneDocument document{world, &bus};

    world::Entity entity(edit::EntityId id) {
        return world.entity(document.entity_of(id));
    }
};

// Lampe mit Schirm darunter.
edit::EntityId build_lamp(Fixture& f) {
    world::Entity lamp = f.world.create("Lampe");
    lamp.set(world::Transform{.position = {2.0, 0.0, 0.0}}).set(Glow{3.0f});
    f.world.create("Schirm").set(world::Transform{}).set_parent(lamp);
    return f.document.id_of(lamp.id());
}

constexpr u64 prefab_id = 777;

} // namespace

TEST_CASE("Linking a subtree to a prefab is undoable", "[edit][prefab]") {
    Fixture f;
    const edit::EntityId lamp = build_lamp(f);
    const world::Prefab prefab = world::make_prefab(f.world, f.document.entity_of(lamp));
    auto command = edit::link_prefab(f.document, lamp, prefab_id);
    REQUIRE(command);
    REQUIRE(f.document.execute(*std::move(command)));
    const u64 root = f.document.entity_of(lamp);
    REQUIRE(f.entity(lamp).get<world::PrefabInstance>() != nullptr);
    CHECK(f.entity(lamp).get<world::PrefabInstance>()->prefab == prefab_id);
    const auto nodes = world::instance_nodes(f.world, root);
    REQUIRE(nodes.size() == 2);
    CHECK(nodes[0].first == 1);
    CHECK(nodes[1].first == 2);
    // Nur die Lage der Wurzel weicht von der neuen Vorlage ab.
    CHECK(world::prefab_overrides(f.world, root, prefab).array()->size() == 1);
    // Schon eine Instanz: nicht noch einmal.
    CHECK_FALSE(edit::link_prefab(f.document, lamp, prefab_id));
    CHECK_FALSE(edit::link_prefab(f.document, lamp, 0));

    REQUIRE(f.document.undo());
    CHECK(f.entity(lamp).get<world::PrefabInstance>() == nullptr);
    CHECK(f.world.entity(nodes[1].second).get<world::PrefabNode>() == nullptr);
    REQUIRE(f.document.redo());
    CHECK(f.entity(lamp).get<world::PrefabInstance>() != nullptr);
}

TEST_CASE("Unpacking a prefab instance is undoable", "[edit][prefab]") {
    Fixture f;
    const edit::EntityId lamp = build_lamp(f);
    REQUIRE(f.document.execute(*edit::link_prefab(f.document, lamp, prefab_id)));
    // Overrides einer Instanz, deren Vorlage fehlte, kommen mit Undo zurück.
    f.entity(lamp).set(world::PrefabOverrides{.json = "[]"});
    const u64 child = f.world.children(f.document.entity_of(lamp)).front();
    const edit::EntityId child_id = f.document.id_of(child);

    // Auch über ein Kind der Instanz nicht direkt aufbrechbar: nur über die Wurzel.
    CHECK_FALSE(edit::unpack_prefab(f.document, child_id));
    auto command = edit::unpack_prefab(f.document, lamp);
    REQUIRE(command);
    REQUIRE(f.document.execute(*std::move(command)));
    CHECK(f.entity(lamp).get<world::PrefabInstance>() == nullptr);
    CHECK(f.entity(lamp).get<world::PrefabOverrides>() == nullptr);
    CHECK(f.entity(child_id).get<world::PrefabNode>() == nullptr);
    CHECK(f.world.entity_count() == 2);

    REQUIRE(f.document.undo());
    CHECK(f.entity(lamp).get<world::PrefabInstance>()->prefab == prefab_id);
    CHECK(f.entity(child_id).get<world::PrefabNode>()->node == 2);
    REQUIRE(f.entity(lamp).get<world::PrefabOverrides>() != nullptr);
    CHECK(f.entity(lamp).get<world::PrefabOverrides>()->json == "[]");
}

TEST_CASE("Prefab snapshots insert new instances with document ids", "[edit][prefab]") {
    Fixture f;
    const edit::EntityId lamp = build_lamp(f);
    const world::Prefab prefab = world::make_prefab(f.world, f.document.entity_of(lamp));
    auto snapshot = edit::prefab_snapshot(f.document, prefab, prefab_id, {5.0, 0.0, -1.0});
    REQUIRE(snapshot);
    REQUIRE(snapshot->nodes.size() == 2);
    const edit::EntityId root = snapshot->nodes[0].id;
    CHECK(snapshot->nodes[0].parent == 0);
    CHECK(snapshot->nodes[1].parent == root);
    CHECK_FALSE(f.document.exists(root));

    REQUIRE(f.document.execute(edit::insert_entities(*std::move(snapshot), "Prefab einfügen")));
    CHECK(f.world.entity_count() == 4);
    world::Entity inserted = f.entity(root);
    REQUIRE(inserted.get<world::PrefabInstance>() != nullptr);
    CHECK(inserted.get<world::PrefabInstance>()->prefab == prefab_id);
    CHECK(inserted.get<world::Transform>()->position.x == 5.0);
    CHECK(inserted.get<Glow>()->intensity == 3.0f);
    CHECK(world::instance_nodes(f.world, inserted.id()).size() == 2);

    REQUIRE(f.document.undo());
    CHECK(f.world.entity_count() == 2);
}

TEST_CASE("Documents save and open prefab instances through their lookup", "[edit][prefab]") {
    Fixture f;
    const edit::EntityId lamp = build_lamp(f);
    const world::Prefab prefab = world::make_prefab(f.world, f.document.entity_of(lamp));
    REQUIRE(f.document.execute(*edit::link_prefab(f.document, lamp, prefab_id)));
    f.document.set_prefabs([&](u64 id) { return id == prefab_id ? &prefab : nullptr; });

    std::random_device random;
    const auto base = std::filesystem::temp_directory_path() /
                      ("ngine_prefab_doc_" + std::to_string(random()) + ".nscene");
    const std::u8string text = base.u8string();
    const std::string path{text.begin(), text.end()};
    REQUIRE(f.document.save(path));
    const std::string before = world::save_scene(f.world, f.document.prefabs());
    auto opened = f.document.open(path);
    REQUIRE(opened);
    CHECK(*opened == 1);
    CHECK(f.world.entity_count() == 2);
    CHECK(world::save_scene(f.world, f.document.prefabs()) == before);
    static_cast<void>(filesystem::remove(path));
}
