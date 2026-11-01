#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

import ngine.core;
import ngine.reflect;
import ngine.world;

using namespace ngine;

namespace {

struct Position {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

struct Velocity {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

// Besitzt Heap-Speicher: prüft, dass flecs Konstruktoren und Destruktoren richtig aufruft, wenn
// Entities zwischen Tabellen wandern.
struct Inventory {
    std::vector<std::string> items;
    std::string owner = "niemand";
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<Position>("Position").field("x", &Position::x).field("y", &Position::y);
    types.add<Velocity>("Velocity").field("x", &Velocity::x).field("y", &Velocity::y);
    types.add<Inventory>("Inventory").field("owner", &Inventory::owner);
    return types;
}

} // namespace

TEST_CASE("Entities hold components", "[world]") {
    const auto types = make_types();
    world::World world{types};

    world::Entity entity = world.create("Spieler");
    CHECK(entity.alive());
    CHECK(entity.name() == "Spieler");
    CHECK_FALSE(entity.has<Position>());

    entity.set(Position{1.0f, 2.0f});
    REQUIRE(entity.has<Position>());
    CHECK(entity.get<Position>()->x == 1.0f);

    entity.get_mut<Position>()->y = 5.0f;
    CHECK(entity.get<Position>()->y == 5.0f);

    entity.set(Position{3.0f, 4.0f});
    CHECK(entity.get<Position>()->x == 3.0f);

    entity.remove<Position>();
    CHECK_FALSE(entity.has<Position>());
    CHECK(entity.get<Position>() == nullptr);
    CHECK(world.entity_count() == 1);
}

TEST_CASE("Destroyed entities are no longer alive", "[world]") {
    const auto types = make_types();
    world::World world{types};

    world::Entity a = world.create();
    const world::Entity copy = a;
    a.destroy();
    CHECK_FALSE(copy.alive());
    CHECK(world.entity_count() == 0);

    // Die Kennung kommt mit neuer Generation wieder; der alte Griff bleibt tot.
    const world::Entity b = world.create();
    CHECK(b.alive());
    CHECK(b != copy);
    CHECK_FALSE(copy.alive());
}

TEST_CASE("Components with heap memory survive table moves", "[world]") {
    const auto types = make_types();
    world::World world{types};

    std::vector<world::Entity> entities;
    for (int i = 0; i < 100; ++i) {
        world::Entity entity = world.create();
        Inventory inventory;
        inventory.owner = "Besitzer mit einem langen Namen Nummer " + std::to_string(i);
        inventory.items.assign(static_cast<usize>(i % 5), "Gegenstand");
        entity.set(std::move(inventory));
        entities.push_back(entity);
    }
    // Weitere Komponenten verschieben die Entities in andere Tabellen.
    for (usize i = 0; i < entities.size(); i += 2) {
        entities[i].set(Position{});
    }
    for (usize i = 0; i < entities.size(); i += 3) {
        entities[i].destroy();
    }
    for (usize i = 0; i < entities.size(); ++i) {
        if (i % 3 == 0) {
            continue;
        }
        const Inventory* inventory = entities[i].get<Inventory>();
        REQUIRE(inventory != nullptr);
        CHECK(inventory->owner == "Besitzer mit einem langen Namen Nummer " + std::to_string(i));
        CHECK(inventory->items.size() == i % 5);
    }
}

TEST_CASE("New components start with their default value", "[world]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity entity = world.create();
    const auto component = world.component<Inventory>();
    const auto* inventory = static_cast<const Inventory*>(world.ensure(entity.id(), component));
    CHECK(inventory->owner == "niemand");
}

TEST_CASE("Queries visit matching entities", "[world]") {
    const auto types = make_types();
    world::World world{types};
    for (int i = 0; i < 10; ++i) {
        world::Entity entity = world.create();
        entity.set(Position{static_cast<f32>(i), 0.0f});
        if (i % 2 == 0) {
            entity.set(Velocity{1.0f, 2.0f});
        }
    }

    auto query = world.query<const Velocity, Position>();
    CHECK(query.count() == 5);
    query.each([](const Velocity& velocity, Position& position) {
        position.x += velocity.x;
        position.y += velocity.y;
    });

    int moved = 0;
    f32 sum = 0.0f;
    world.each<const Position>([&](world::Entity entity, const Position& position) {
        CHECK(entity.alive());
        sum += position.x;
        if (position.y == 2.0f) {
            ++moved;
        }
    });
    CHECK(moved == 5);
    CHECK(sum == 45.0f + 5.0f);
}

TEST_CASE("Structural changes inside each take effect afterwards", "[world]") {
    const auto types = make_types();
    world::World world{types};
    for (int i = 0; i < 4; ++i) {
        world.create().set(Position{});
    }
    world.each<Position>([](world::Entity entity, Position&) { entity.set(Velocity{}); });
    CHECK(world.query<const Velocity>().count() == 4);
}

TEST_CASE("Hierarchy keeps parents and children", "[world]") {
    const auto types = make_types();
    world::World world{types};

    world::Entity sun = world.create("Sonne");
    world::Entity earth = world.create("Erde");
    world::Entity moon = world.create("Mond");
    world::Entity mars = world.create("Mars");
    earth.set_parent(sun);
    moon.set_parent(earth);
    mars.set_parent(sun);

    CHECK(moon.parent() == earth);
    CHECK(earth.parent() == sun);
    CHECK_FALSE(sun.parent());
    CHECK(sun.children() == std::vector<world::Entity>{earth, mars});

    // Ein neues Elternteil ersetzt das alte.
    moon.set_parent(mars);
    CHECK(moon.parent() == mars);
    CHECK(earth.children().empty());

    moon.set_parent({});
    CHECK_FALSE(moon.parent());

    // Mit dem Elternteil verschwinden die Kinder.
    moon.set_parent(earth);
    sun.destroy();
    CHECK_FALSE(earth.alive());
    CHECK_FALSE(moon.alive());
    CHECK_FALSE(mars.alive());
    CHECK(world.entity_count() == 0);
}

TEST_CASE("Components can be visited without knowing their type", "[world]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity entity = world.create("Kiste");
    entity.set(Position{1.0f, 2.0f});
    world::Entity child = world.create();
    child.set_parent(entity);

    std::vector<std::string> names;
    auto visit = [&](const reflect::TypeInfo& type, world::ComponentId, const void*) {
        names.push_back(type.name);
    };
    world.for_each_component(entity.id(), {.context = &visit,
                                           .visit = [](void* context, const reflect::TypeInfo& type,
                                                       world::ComponentId id, const void* data) {
                                               (*static_cast<decltype(visit)*>(context))(type, id,
                                                                                         data);
                                           }});
    // Die Beziehung zum Elternteil ist keine Komponente.
    CHECK(names == std::vector<std::string>{"Name", "Position"});
}
