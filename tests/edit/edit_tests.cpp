#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.world;
import ngine.edit;

using namespace ngine;
using Catch::Approx;

namespace {

enum class Shape : u8 { Point, Spot, Area };

struct Lamp {
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    f32 angle = 0.5f;
    Shape shape = Shape::Point;
    bool on = true;
    std::string label;
};

struct Marker {
    i32 value = 0;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<Shape>("Shape")
        .value("Point", Shape::Point)
        .value("Spot", Shape::Spot)
        .value("Area", Shape::Area);
    types.add<Lamp>("Lamp")
        .field("color", &Lamp::color)
        .hint(reflect::Hint::Color)
        .field("intensity", &Lamp::intensity)
        .range(0.0, 10.0)
        .field("angle", &Lamp::angle)
        .hint(reflect::Hint::Angle)
        .field("shape", &Lamp::shape)
        .field("on", &Lamp::on)
        .field("label", &Lamp::label);
    types.add<Marker>("Marker").field("value", &Marker::value);
    return types;
}

// Welt mit Dokument und Ereignissen für einen Test.
struct Fixture {
    reflect::TypeRegistry types = make_types();
    world::World world{types};
    events::EventBus bus;
    edit::SceneDocument document{world, &bus};

    edit::EntityId add(std::string name, edit::EntityId parent = 0) {
        auto created = edit::create_entity(document, std::move(name), parent);
        REQUIRE(document.execute(std::move(created.command)));
        return created.id;
    }
    world::Entity entity(edit::EntityId id) {
        return world.entity(document.entity_of(id));
    }
    const edit::InspectorRow& row(const std::vector<edit::InspectorRow>& rows,
                                  const std::string& label) {
        const auto it = std::ranges::find(rows, label, &edit::InspectorRow::label);
        REQUIRE(it != rows.end());
        return *it;
    }
};

} // namespace

TEST_CASE("Created entities get stable ids and undo removes them", "[edit]") {
    Fixture f;
    const edit::EntityId lamp = f.add("Lampe");
    REQUIRE(f.document.exists(lamp));
    CHECK(f.entity(lamp).name() == "Lampe");
    CHECK(f.entity(lamp).has<world::Transform>());
    CHECK(f.document.dirty());
    CHECK(f.document.history().undo_label() == "'Lampe' anlegen");

    REQUIRE(f.document.undo());
    CHECK_FALSE(f.document.exists(lamp));
    CHECK(f.world.entity_count() == 0);
    CHECK_FALSE(f.document.dirty());

    REQUIRE(f.document.redo());
    CHECK(f.document.exists(lamp));
    CHECK(f.entity(lamp).name() == "Lampe");
}

TEST_CASE("Field edits are undone and redone through reflected bytes", "[edit]") {
    Fixture f;
    const edit::EntityId id = f.add("Lampe");
    REQUIRE(f.document.execute(edit::add_component(id, f.types.get<Lamp>().id)));

    const auto rows = edit::build_rows(f.types.get<Lamp>());
    const edit::InspectorRow& intensity = f.row(rows, "intensity");
    const f32 value = 4.0f;
    REQUIRE(edit::commit(f.document, id, intensity, &value) == true);
    CHECK(f.entity(id).get<Lamp>()->intensity == 4.0f);

    // Gleicher Wert: kein neuer Schritt.
    const usize steps = f.document.history().size();
    REQUIRE(edit::commit(f.document, id, intensity, &value) == false);
    CHECK(f.document.history().size() == steps);

    REQUIRE(f.document.undo());
    CHECK(f.entity(id).get<Lamp>()->intensity == 1.0f);
    REQUIRE(f.document.redo());
    CHECK(f.entity(id).get<Lamp>()->intensity == 4.0f);

    const std::string label = "Studio";
    REQUIRE(edit::commit(f.document, id, f.row(rows, "label"), &label) == true);
    CHECK(f.entity(id).get<Lamp>()->label == "Studio");
    REQUIRE(f.document.undo());
    CHECK(f.entity(id).get<Lamp>()->label.empty());
}

TEST_CASE("Consecutive edits of one field merge until the history is sealed", "[edit]") {
    Fixture f;
    const edit::EntityId id = f.add("Lampe");
    REQUIRE(f.document.execute(edit::add_component(id, f.types.get<Lamp>().id)));
    const auto rows = edit::build_rows(f.types.get<Lamp>());
    const edit::InspectorRow& intensity = f.row(rows, "intensity");

    const usize before = f.document.history().size();
    for (const f32 value : {2.0f, 3.0f, 5.0f}) {
        REQUIRE(edit::commit(f.document, id, intensity, &value));
    }
    CHECK(f.document.history().size() == before + 1);
    f.document.history().seal();
    const f32 last = 6.0f;
    REQUIRE(edit::commit(f.document, id, intensity, &last));
    CHECK(f.document.history().size() == before + 2);

    REQUIRE(f.document.undo());
    CHECK(f.entity(id).get<Lamp>()->intensity == 5.0f);
    REQUIRE(f.document.undo());
    CHECK(f.entity(id).get<Lamp>()->intensity == 1.0f);
}

TEST_CASE("Inspector rows come from reflection alone", "[edit]") {
    Fixture f;
    const auto lamp = edit::build_rows(f.types.get<Lamp>());
    REQUIRE(lamp.size() == 6);
    CHECK(f.row(lamp, "color").widget == edit::Widget::Color);
    CHECK(f.row(lamp, "color").count == 3);
    CHECK(f.row(lamp, "intensity").widget == edit::Widget::Number);
    CHECK(f.row(lamp, "intensity").scalar == edit::Scalar::F32);
    REQUIRE(f.row(lamp, "intensity").range);
    CHECK(f.row(lamp, "intensity").range->max == 10.0);
    CHECK(f.row(lamp, "angle").widget == edit::Widget::Angle);
    CHECK(f.row(lamp, "shape").widget == edit::Widget::Enum);
    CHECK(f.row(lamp, "on").widget == edit::Widget::Checkbox);
    CHECK(f.row(lamp, "label").widget == edit::Widget::Text);

    const auto transform = edit::build_rows(f.types.get<world::Transform>());
    REQUIRE(transform.size() == 3);
    CHECK(transform[0].widget == edit::Widget::Vector);
    CHECK(transform[0].scalar == edit::Scalar::F64);
    CHECK(transform[0].count == 3);
    CHECK(transform[1].widget == edit::Widget::Rotation);
    CHECK(transform[2].widget == edit::Widget::Vector);
    CHECK(edit::to_string(f.types, transform[0].path) == "Transform.position");
}

TEST_CASE("Inspector clamps numbers to the reflected range", "[edit]") {
    Fixture f;
    const edit::EntityId id = f.add("Lampe");
    REQUIRE(f.document.execute(edit::add_component(id, f.types.get<Lamp>().id)));
    const auto rows = edit::build_rows(f.types.get<Lamp>());
    const f32 too_much = 50.0f;
    REQUIRE(edit::commit(f.document, id, f.row(rows, "intensity"), &too_much));
    CHECK(f.entity(id).get<Lamp>()->intensity == 10.0f);
    const f32 negative = -3.0f;
    REQUIRE(edit::commit(f.document, id, f.row(rows, "intensity"), &negative));
    CHECK(f.entity(id).get<Lamp>()->intensity == 0.0f);
}

TEST_CASE("Inspector edits rotations as Euler angles in degrees", "[edit]") {
    Fixture f;
    const edit::EntityId id = f.add("Würfel");
    const auto rows = edit::build_rows(f.types.get<world::Transform>());
    const edit::InspectorRow& rotation = f.row(rows, "rotation");
    REQUIRE(edit::commit_rotation(f.document, id, rotation, {30.0f, 45.0f, 10.0f}));
    const math::Vec3 degrees =
        edit::rotation_degrees(f.entity(id).get<world::Transform>()->rotation);
    CHECK(degrees.x == Approx(30.0f).margin(1e-3));
    CHECK(degrees.y == Approx(45.0f).margin(1e-3));
    CHECK(degrees.z == Approx(10.0f).margin(1e-3));
    REQUIRE(f.document.undo());
    CHECK(f.entity(id).get<world::Transform>()->rotation == math::Quat{});
}

TEST_CASE("Inspector lists components with fields in registration order", "[edit]") {
    Fixture f;
    const edit::EntityId id = f.add("Lampe");
    REQUIRE(f.document.execute(edit::add_component(id, f.types.get<Marker>().id)));
    REQUIRE(f.document.execute(edit::add_component(id, f.types.get<Lamp>().id)));
    const auto components = edit::inspect(f.document, id);
    std::vector<std::string> names;
    for (const auto& component : components) {
        names.push_back(component.type->name);
    }
    // WorldTransform hat keine Felder und fehlt.
    CHECK(names == std::vector<std::string>{"Name", "Transform", "Lamp", "Marker"});

    const auto addable = edit::addable_components(f.document, id);
    CHECK(std::ranges::find(addable, &f.types.get<Lamp>()) == addable.end());
    CHECK(std::ranges::find(addable, &f.types.get<math::Vec3>()) == addable.end());
    CHECK(std::ranges::find(addable, &f.types.get<world::WorldTransform>()) == addable.end());

    REQUIRE(f.document.execute(edit::remove_component(id, f.types.get<Lamp>().id)));
    CHECK_FALSE(f.entity(id).has<Lamp>());
    const auto after = edit::addable_components(f.document, id);
    CHECK(std::ranges::find(after, &f.types.get<Lamp>()) != after.end());
}

TEST_CASE("Removed components come back with their values", "[edit]") {
    Fixture f;
    const edit::EntityId id = f.add("Lampe");
    f.entity(id).set(Lamp{.intensity = 7.0f, .shape = Shape::Area, .label = "Fenster"});
    f.document.reset_history();
    REQUIRE(f.document.execute(edit::remove_component(id, f.types.get<Lamp>().id)));
    REQUIRE(f.document.undo());
    const Lamp* lamp = f.entity(id).get<Lamp>();
    REQUIRE(lamp != nullptr);
    CHECK(lamp->intensity == 7.0f);
    CHECK(lamp->shape == Shape::Area);
    CHECK(lamp->label == "Fenster");
}

TEST_CASE("Rename and reparent are undoable and refuse cycles", "[edit]") {
    Fixture f;
    const edit::EntityId root = f.add("Wurzel");
    const edit::EntityId child = f.add("Kind", root);
    const edit::EntityId other = f.add("Anderes");

    REQUIRE(f.document.execute(edit::rename(child, "Neu")));
    CHECK(f.entity(child).name() == "Neu");
    REQUIRE(f.document.undo());
    CHECK(f.entity(child).name() == "Kind");

    CHECK_FALSE(edit::can_reparent(f.document, root, child));
    CHECK_FALSE(edit::can_reparent(f.document, root, root));
    CHECK_FALSE(edit::can_reparent(f.document, child, root));
    CHECK(edit::can_reparent(f.document, child, other));
    CHECK(edit::can_reparent(f.document, child, 0));
    CHECK_FALSE(f.document.execute(edit::reparent(root, child)));

    REQUIRE(f.document.execute(edit::reparent(child, other)));
    CHECK(f.entity(child).parent() == f.entity(other));
    REQUIRE(f.document.execute(edit::reparent(child, 0)));
    CHECK_FALSE(f.entity(child).parent());
    REQUIRE(f.document.undo());
    REQUIRE(f.document.undo());
    CHECK(f.entity(child).parent() == f.entity(root));
}

TEST_CASE("Deleting a subtree is undone with the same stable ids", "[edit]") {
    Fixture f;
    const edit::EntityId root = f.add("Wurzel");
    const edit::EntityId child = f.add("Kind", root);
    const edit::EntityId grandchild = f.add("Enkel", child);
    f.entity(grandchild).set(Lamp{.intensity = 3.0f, .label = "Enkel-Lampe"});
    const auto rows = edit::build_rows(f.types.get<Lamp>());

    REQUIRE(f.document.execute(edit::destroy_entity(child)));
    CHECK_FALSE(f.document.exists(child));
    CHECK_FALSE(f.document.exists(grandchild));
    CHECK(f.world.entity_count() == 1);

    REQUIRE(f.document.undo());
    REQUIRE(f.document.exists(child));
    REQUIRE(f.document.exists(grandchild));
    CHECK(f.entity(child).parent() == f.entity(root));
    CHECK(f.entity(grandchild).parent() == f.entity(child));
    CHECK(f.entity(grandchild).get<Lamp>()->label == "Enkel-Lampe");

    // Ein älterer Befehl, der auf das Enkelkind verweist, funktioniert weiter.
    const f32 value = 9.0f;
    REQUIRE(edit::commit(f.document, grandchild, f.row(rows, "intensity"), &value));
    REQUIRE(f.document.undo());
    CHECK(f.entity(grandchild).get<Lamp>()->intensity == 3.0f);

    // Die neue Änderung hat das Redo des Löschens verworfen. Alles zurück und wieder vor: Die
    // Entities entstehen mit ihren alten Kennungen neu.
    while (f.document.undo()) {
    }
    CHECK(f.world.entity_count() == 0);
    while (f.document.redo()) {
    }
    CHECK(f.world.entity_count() == 3);
    CHECK(f.entity(grandchild).parent() == f.entity(child));
}

TEST_CASE("Groups apply all or nothing", "[edit]") {
    Fixture f;
    const edit::EntityId id = f.add("Lampe");
    std::vector<std::unique_ptr<edit::Command>> commands;
    commands.push_back(edit::rename(id, "Gruppe"));
    commands.push_back(edit::add_component(id, f.types.get<Marker>().id));
    commands.push_back(edit::add_component(id, f.types.get<Marker>().id));
    CHECK_FALSE(f.document.execute(edit::group("Doppelt", std::move(commands))));
    CHECK(f.entity(id).name() == "Lampe");
    CHECK_FALSE(f.entity(id).has<Marker>());
}

TEST_CASE("Saving marks the document clean and undo past it makes it dirty", "[edit]") {
    Fixture f;
    std::vector<bool> dirty_events;
    auto subscription = f.bus.subscribe<edit::DirtyChanged>(
        [&](const edit::DirtyChanged& e) { dirty_events.push_back(e.dirty); });
    const edit::EntityId id = f.add("Lampe");
    const std::string path = "edit_test_scene.json";
    REQUIRE(f.document.save(path));
    CHECK_FALSE(f.document.dirty());
    REQUIRE(f.document.execute(edit::rename(id, "Andere")));
    CHECK(f.document.dirty());
    REQUIRE(f.document.undo());
    CHECK_FALSE(f.document.dirty());
    REQUIRE(f.document.undo());
    CHECK(f.document.dirty());
    CHECK(dirty_events == std::vector<bool>{true, false, true, false, true});
    std::remove(path.c_str());
}

TEST_CASE("Opened scenes save to identical text without changes", "[edit][scene]") {
    Fixture f;
    const edit::EntityId sun = f.add("Sonne");
    const edit::EntityId lamp = f.add("Lampe", sun);
    f.entity(lamp).set(Lamp{.color = {0.25f, 0.5f, 1.0f}, .intensity = 2.5f, .shape = Shape::Spot});
    f.entity(sun).set(Marker{42});
    const std::string original = world::save_scene(f.world);

    // Eine zweite Welt meldet die Komponenten in anderer Reihenfolge an.
    reflect::TypeRegistry types = make_types();
    world::World other{types};
    other.create("Vorher").set(Marker{1}).set(Lamp{});
    events::EventBus bus;
    edit::SceneDocument document{other, &bus};
    const std::string path = "edit_test_roundtrip.json";
    REQUIRE(f.document.save(path));
    auto opened = document.open(path);
    REQUIRE(opened);
    CHECK(*opened == 2);
    CHECK_FALSE(document.dirty());
    CHECK(world::save_scene(other) == original);
    std::remove(path.c_str());
}

TEST_CASE("Opening a missing scene leaves an empty document", "[edit][scene]") {
    Fixture f;
    f.add("Lampe");
    CHECK_FALSE(f.document.open("gibt-es-nicht.json"));
    CHECK(f.world.entity_count() == 0);
    CHECK_FALSE(f.document.history().can_undo());
}

TEST_CASE("Scene tree lists entities parent first with depth", "[edit]") {
    Fixture f;
    const edit::EntityId a = f.add("A");
    f.add("A1", a);
    const edit::EntityId b = f.add("B");
    const edit::EntityId unnamed = f.add("");
    const auto rows = edit::build_tree(f.document);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].name == "A");
    CHECK(rows[0].has_children);
    CHECK(rows[1].name == "A1");
    CHECK(rows[1].depth == 1);
    CHECK(rows[2].id == b);
    CHECK_FALSE(rows[2].has_children);
    CHECK(rows[3].name == std::string{"Entity "} + std::to_string(unnamed));
}

TEST_CASE("Selection publishes changes and drops deleted entities", "[edit]") {
    Fixture f;
    std::vector<edit::SelectionChanged> changes;
    auto subscription = f.bus.subscribe<edit::SelectionChanged>(
        [&](const edit::SelectionChanged& e) { changes.push_back(e); });
    edit::Selection selection{&f.bus};
    const edit::EntityId id = f.add("Lampe");
    selection.select(id);
    selection.select(id);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].current == id);
    REQUIRE(f.document.execute(edit::destroy_entity(id)));
    selection.validate(f.document);
    CHECK(selection.current() == 0);
    CHECK(changes.size() == 2);
}

TEST_CASE("Document changes are announced as events", "[edit]") {
    Fixture f;
    usize changes = 0;
    usize resets = 0;
    auto changed =
        f.bus.subscribe<edit::EntityChanged>([&](const edit::EntityChanged&) { ++changes; });
    auto reset =
        f.bus.subscribe<edit::DocumentReset>([&](const edit::DocumentReset&) { ++resets; });
    const edit::EntityId id = f.add("Lampe");
    REQUIRE(f.document.execute(edit::rename(id, "X")));
    REQUIRE(f.document.undo());
    CHECK(changes == 3);
    f.document.clear();
    CHECK(resets == 1);
    CHECK(f.world.entity_count() == 0);
}

TEST_CASE("Inserted subtrees are undone and redone as one step", "[edit]") {
    Fixture f;
    const edit::EntityId source = f.add("Vorlage");
    const edit::EntityId source_child = f.add("Teil", source);
    f.entity(source_child).set(Lamp{.intensity = 2.0f, .label = "Teil-Lampe"});

    // Kopie des Teilbaums mit freien Kennungen, wie der Asset-Browser sie baut.
    edit::EntitySnapshot copy = edit::snapshot(f.document, source);
    REQUIRE(copy.nodes.size() == 2);
    const edit::EntityId root = f.document.reserve_id();
    const edit::EntityId child = f.document.reserve_id();
    copy.nodes[0].id = root;
    copy.nodes[0].parent = 0;
    copy.nodes[1].id = child;
    copy.nodes[1].parent = root;

    REQUIRE(f.document.execute(edit::insert_entities(copy, "Modell einfügen")));
    REQUIRE(f.document.exists(root));
    REQUIRE(f.document.exists(child));
    CHECK(f.entity(child).parent() == f.entity(root));
    CHECK(f.entity(child).get<Lamp>()->label == "Teil-Lampe");
    CHECK(f.world.entity_count() == 4);

    REQUIRE(f.document.undo());
    CHECK_FALSE(f.document.exists(root));
    CHECK_FALSE(f.document.exists(child));
    CHECK(f.world.entity_count() == 2);

    REQUIRE(f.document.redo());
    CHECK(f.document.exists(child));
    CHECK(f.world.entity_count() == 4);

    // Kennungen, die schon vergeben sind, lehnt der Befehl ab, ohne etwas zu hinterlassen.
    REQUIRE_FALSE(f.document.execute(edit::insert_entities(copy, "Doppelt")));
    CHECK(f.world.entity_count() == 4);
}
