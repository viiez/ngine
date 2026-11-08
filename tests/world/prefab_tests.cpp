#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

import ngine.core;
import ngine.reflect;
import ngine.filesystem;
import ngine.math;
import ngine.serialize;
import ngine.world;

using namespace ngine;

namespace {

struct Lamp {
    f32 intensity = 1.0f;
    f32 range = 5.0f;
    std::string color = "warm";
};

struct Tag {
    i32 value = 0;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<Lamp>("Lamp")
        .field("intensity", &Lamp::intensity)
        .field("range", &Lamp::range)
        .field("color", &Lamp::color);
    types.add<Tag>("Tag").field("value", &Tag::value);
    return types;
}

constexpr u64 lantern_id = 4242;

// Laterne: Wurzel mit Pfahl und Leuchte darunter.
constexpr std::string_view lantern_text = R"({
  "format": "ngine.prefab", "version": 1,
  "entities": [
    { "id": 1, "components": { "Name": { "value": "Laterne" }, "Transform": {} } },
    { "id": 2, "parent": 1, "components": { "Name": { "value": "Pfahl" },
      "Transform": { "position": { "x": 0, "y": 1.5, "z": 0 } } } },
    { "id": 3, "parent": 1, "components": { "Name": { "value": "Leuchte" },
      "Transform": { "position": { "x": 0, "y": 3, "z": 0 } }, "Lamp": { "intensity": 6, "color": "warm" } } }
  ]
})";

world::Prefab lantern() {
    auto prefab = world::parse_prefab(lantern_text);
    REQUIRE(prefab);
    return *std::move(prefab);
}

// Entity einer Instanz zu einem Knoten.
u64 node_entity(const world::World& world, u64 root, u32 node) {
    for (const auto& [id, entity] : world::instance_nodes(world, root)) {
        if (id == node) {
            return entity;
        }
    }
    return 0;
}

class TempDir {
public:
    TempDir() {
        std::random_device random;
        const auto base =
            std::filesystem::temp_directory_path() / ("ngine_prefab_" + std::to_string(random()));
        const std::u8string text = base.u8string();
        path_.assign(text.begin(), text.end());
        static_cast<void>(filesystem::create_directories(path_));
    }
    ~TempDir() {
        static_cast<void>(filesystem::remove(path_));
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string file(std::string_view name) const {
        return filesystem::join(path_, name);
    }

private:
    std::string path_;
};

} // namespace

TEST_CASE("Prefab files are parsed and written", "[world][prefab]") {
    const world::Prefab prefab = lantern();
    REQUIRE(prefab.nodes.size() == 3);
    CHECK(prefab.nodes[0].id == 1);
    CHECK_FALSE(prefab.nodes[0].parent);
    CHECK(prefab.nodes[2].parent == std::optional<usize>{0});
    REQUIRE(prefab.find(3) != nullptr);
    CHECK(prefab.find(9) == nullptr);
    // Schreiben und wieder lesen ergibt denselben Text.
    const std::string text = world::write_prefab(prefab);
    auto again = world::parse_prefab(text);
    REQUIRE(again);
    CHECK(world::write_prefab(*again) == text);

    CHECK_FALSE(world::parse_prefab("kein json"));
    CHECK_FALSE(world::parse_prefab(R"({"format":"ngine.scene","version":1,"entities":[]})"));
    CHECK_FALSE(world::parse_prefab(R"({"format":"ngine.prefab","version":1,"entities":[]})"));
    // Zwei Wurzeln.
    CHECK_FALSE(world::parse_prefab(
        R"({"format":"ngine.prefab","version":1,"entities":[{"id":1},{"id":2}]})"));
    // Elternteil nach dem Kind.
    CHECK_FALSE(world::parse_prefab(
        R"({"format":"ngine.prefab","version":1,"entities":[{"id":1},{"id":2,"parent":3},{"id":3,"parent":1}]})"));
}

TEST_CASE("Prefab instances get nodes, values and overrides", "[world][prefab]") {
    const auto types = make_types();
    world::World world{types};
    const world::Prefab prefab = lantern();
    json::Value overrides = *json::parse(R"([{"node":3,"components":{"Lamp":{"color":"kalt"}}}])");
    auto root = world::instantiate(world, prefab, lantern_id, &overrides);
    REQUIRE(root);
    CHECK(world.entity_count() == 3);
    CHECK(root->get<world::PrefabInstance>()->prefab == lantern_id);
    CHECK(root->get<world::PrefabNode>()->node == 1);
    const u64 bulb = node_entity(world, root->id(), 3);
    REQUIRE(bulb != 0);
    CHECK(world.parent(bulb) == root->id());
    const Lamp* lamp = world.entity(bulb).get<Lamp>();
    REQUIRE(lamp != nullptr);
    CHECK(lamp->intensity == 6.0f);
    CHECK(lamp->color == "kalt");
    CHECK(world::instance_root(world, bulb) == root->id());
    CHECK(world::instance_root(world, world.create().id()) == 0);

    // Überschreibungen sind genau die abweichenden Felder.
    const json::Value found = world::prefab_overrides(world, root->id(), prefab);
    REQUIRE(found.array() != nullptr);
    REQUIRE(found.array()->size() == 1);
    CHECK(json::write(found) == json::write(overrides));

    // Ohne Abweichung ein leeres Array.
    auto plain = world::instantiate(world, prefab, lantern_id);
    REQUIRE(plain);
    CHECK(world::prefab_overrides(world, plain->id(), prefab).array()->empty());
}

TEST_CASE("Changed prefabs update all instances and keep overrides", "[world][prefab]") {
    const auto types = make_types();
    world::World world{types};
    const world::Prefab before = lantern();
    auto first = world::instantiate(world, before, lantern_id);
    auto second = world::instantiate(world, before, lantern_id);
    REQUIRE(first);
    REQUIRE(second);
    // Die zweite Laterne steht woanders, leuchtet kalt und hat eine eigene Komponente.
    world::Transform moved = *second->get<world::Transform>();
    moved.position = {10.0, 0.0, 4.0};
    second->set(moved);
    const u64 second_bulb = node_entity(world, second->id(), 3);
    world.entity(second_bulb).get_mut<Lamp>()->color = "kalt";
    world.entity(second_bulb).set(Tag{7});
    // Ein Kind, das nicht zur Vorlage gehört, hängt unter dem Pfahl.
    const u64 second_pole = node_entity(world, second->id(), 2);
    const u64 extra = world.create("Schild").id();
    world.set_parent(extra, second_pole);

    // Neue Vorlage: heller, roter Standard, ein neuer Knoten (Sockel), Pfahl bleibt.
    world::Prefab after = before;
    after.nodes[2].components["Lamp"]["intensity"] = 9.0;
    after.nodes[2].components["Lamp"]["color"] = "rot";
    world::Prefab::Node base{.id = 4, .parent = 0, .components = {}};
    base.components["Name"]["value"] = "Sockel";
    after.nodes.push_back(base);

    CHECK(world::update_instances(world, lantern_id, before, after) == 2);
    CHECK(world.entity_count() == 9); // 2 × 4 Knoten und das Schild

    const Lamp* first_lamp = world.entity(node_entity(world, first->id(), 3)).get<Lamp>();
    CHECK(first_lamp->intensity == 9.0f);
    CHECK(first_lamp->color == "rot");
    // Die zweite behält ihre Farbe, ihre Lage und ihre eigene Komponente, bekommt aber die neue
    // Helligkeit; dieselben Entities wie vorher.
    CHECK(node_entity(world, second->id(), 3) == second_bulb);
    const Lamp* second_lamp = world.entity(second_bulb).get<Lamp>();
    CHECK(second_lamp->intensity == 9.0f);
    CHECK(second_lamp->color == "kalt");
    CHECK(world.entity(second_bulb).get<Tag>()->value == 7);
    CHECK(second->get<world::Transform>()->position.x == 10.0);
    CHECK(world.parent(extra) == second_pole);
    const u64 second_base = node_entity(world, second->id(), 4);
    REQUIRE(second_base != 0);
    CHECK(world.entity(second_base).name() == "Sockel");
    CHECK(world.parent(second_base) == second->id());

    // Der Pfahl fällt weg: mit allem, was darunter hängt.
    world::Prefab without_pole = after;
    without_pole.nodes.erase(without_pole.nodes.begin() + 1);
    without_pole.nodes[1].parent = 0;
    without_pole.nodes[2].parent = 0;
    CHECK(world::update_instances(world, lantern_id, after, without_pole) == 2);
    CHECK_FALSE(world.alive(second_pole));
    CHECK_FALSE(world.alive(extra));
    CHECK(world.alive(second_bulb));
    CHECK(world.entity_count() == 6);

    // Eine Komponente fällt aus der Vorlage: weg, außer die Instanz überschreibt sie.
    world::Prefab no_lamp = without_pole;
    std::erase_if(*no_lamp.nodes[1].components.object(),
                  [](const json::Member& member) { return member.key == "Lamp"; });
    REQUIRE(no_lamp.nodes[1].id == 3);
    CHECK(world::update_instances(world, lantern_id, without_pole, no_lamp) == 2);
    CHECK(world.entity(node_entity(world, first->id(), 3)).get<Lamp>() == nullptr);
    REQUIRE(world.entity(second_bulb).get<Lamp>() != nullptr);
    CHECK(world.entity(second_bulb).get<Lamp>()->color == "kalt");
}

TEST_CASE("Unpacked instances keep their entities without prefab links", "[world][prefab]") {
    const auto types = make_types();
    world::World world{types};
    auto root = world::instantiate(world, lantern(), lantern_id);
    INFO((root ? std::string{} : root.error()));
    REQUIRE(root);
    const u64 bulb = node_entity(world, root->id(), 3);
    world::unpack_instance(world, root->id());
    CHECK(world.entity_count() == 3);
    CHECK(root->get<world::PrefabInstance>() == nullptr);
    CHECK(world.entity(bulb).get<world::PrefabNode>() == nullptr);
    CHECK(world.entity(bulb).get<Lamp>()->intensity == 6.0f);
    CHECK(world::instance_root(world, bulb) == 0);
}

TEST_CASE("Prefabs made from a subtree start at the origin", "[world][prefab]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity root = world.create("Kiste");
    root.set(world::Transform{.position = {5.0, 1.0, -2.0}});
    world::Entity child = world.create("Deckel");
    child.set(world::Transform{.position = {0.0, 1.0, 0.0}}).set(Tag{3}).set_parent(root);
    const world::Prefab prefab = world::make_prefab(world, root.id());
    REQUIRE(prefab.nodes.size() == 2);
    CHECK(prefab.nodes[1].parent == std::optional<usize>{0});
    // Instanz an derselben Stelle: Nur die Lage der Wurzel weicht ab.
    root.set(world::PrefabInstance{.prefab = 1}).set(world::PrefabNode{.node = 1});
    child.set(world::PrefabNode{.node = 2});
    const json::Value overrides = world::prefab_overrides(world, root.id(), prefab);
    REQUIRE(overrides.array()->size() == 1);
    const json::Value& entry = (*overrides.array())[0];
    CHECK(entry.find("node")->to_u64() == std::optional<u64>{1});
    CHECK(entry.find("components")->find("Transform")->find("position") != nullptr);
    CHECK(entry.find("components")->find("Transform")->find("scale") == nullptr);
}

TEST_CASE("Scenes store prefab instances as id and overrides", "[world][prefab][scene]") {
    const auto types = make_types();
    const world::Prefab prefab = lantern();
    const world::PrefabLookup lookup = [&](u64 id) { return id == lantern_id ? &prefab : nullptr; };
    world::World original{types};
    auto root = world::instantiate(original, prefab, lantern_id);
    REQUIRE(root);
    world::Transform moved = *root->get<world::Transform>();
    moved.position = {3.0, 0.0, 1.0};
    root->set(moved);
    original.entity(node_entity(original, root->id(), 3)).get_mut<Lamp>()->color = "kalt";
    // Eine eigene Entity unter der Leuchte und eine ohne Bezug.
    original.set_parent(original.create("Funke").id(), node_entity(original, root->id(), 3));
    original.create("Baum").set(Tag{1});

    const std::string text = world::save_scene(original, lookup);
    // Die Knoten der Vorlage stehen nicht einzeln in der Datei.
    CHECK(text.find("\"prefab\"") != std::string::npos);
    CHECK(text.find("Pfahl") == std::string::npos);
    CHECK(text.find("kalt") != std::string::npos);

    world::World loaded{types};
    auto count = world::load_scene(loaded, text, lookup);
    REQUIRE(count);
    CHECK(loaded.entity_count() == 5);
    CHECK(world::save_scene(loaded, lookup) == text);

    // Ohne Vorlage bleibt die Instanz als Wurzel mit ihren Überschreibungen erhalten; was unter
    // ihren Knoten hing, hängt an der Wurzel.
    world::World without{types};
    auto partial = world::load_scene(without, text);
    REQUIRE(partial);
    CHECK(without.entity_count() == 3);
    const std::string kept = world::save_scene(without);
    CHECK(kept.find("\"prefab\"") != std::string::npos);
    CHECK(kept.find("kalt") != std::string::npos);
    world::World restored{types};
    REQUIRE(world::load_scene(restored, kept, lookup));
    CHECK(restored.entity_count() == 5);

    // Binär: dasselbe Ergebnis.
    const std::vector<std::byte> binary = world::save_scene_binary(original, lookup);
    world::World from_binary{types};
    auto binary_count = world::load_scene_binary(from_binary, binary, lookup);
    REQUIRE(binary_count);
    CHECK(world::save_scene(from_binary, lookup) == text);
}

TEST_CASE("Binary scenes round trip and reject foreign data", "[world][scene]") {
    const auto types = make_types();
    world::World original{types};
    world::Entity a = original.create("A");
    a.set(world::Transform{.position = {1.0, 2.0, 3.0}})
        .set(Lamp{.intensity = 2.0f, .range = 3.0f, .color = "grün"});
    original.create("B").set(Tag{-5}).set_parent(a);
    const std::string text = world::save_scene(original);

    const std::vector<std::byte> binary = world::save_scene_binary(original);
    world::World loaded{types};
    auto count = world::load_scene_binary(loaded, binary);
    REQUIRE(count);
    CHECK(*count == 2);
    CHECK(world::save_scene(loaded) == text);

    CHECK_FALSE(world::load_scene_binary(loaded, std::span{binary}.first(binary.size() - 3)));
    std::vector<std::byte> wrong = binary;
    wrong[1] = std::byte{0};
    world::World other{types};
    CHECK_FALSE(world::load_scene_binary(other, wrong));
    CHECK(other.entity_count() == 0);

    // Dateien: Endung bestimmt das Format beim Schreiben, der Inhalt beim Lesen.
    const TempDir dir;
    const std::string binary_path = dir.file("szene.nscene");
    REQUIRE(world::save_scene_file(original, binary_path));
    auto bytes = filesystem::read_file(binary_path);
    REQUIRE(bytes);
    CHECK(*bytes == binary);
    world::World from_file{types};
    REQUIRE(world::load_scene_file(from_file, binary_path));
    CHECK(world::save_scene(from_file) == text);
    CHECK(world::is_binary_scene_path("a/b.nscene"));
    CHECK_FALSE(world::is_binary_scene_path("a/b.json"));
}

TEST_CASE("Prefab files load from disk", "[world][prefab]") {
    const TempDir dir;
    const std::string path = dir.file("laterne.prefab");
    REQUIRE(filesystem::write_text(path, lantern_text));
    auto prefab = world::load_prefab_file(path);
    REQUIRE(prefab);
    CHECK(prefab->nodes.size() == 3);
    CHECK_FALSE(world::load_prefab_file(dir.file("fehlt.prefab")));
}
