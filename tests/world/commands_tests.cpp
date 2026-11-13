#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

import ngine.core;
import ngine.reflect;
import ngine.jobs;
import ngine.world;

using namespace ngine;

namespace {

struct Spawner {
    i32 count = 0;
};
struct Child {
    i32 number = 0;
    std::string label;
};
struct Lifetime {
    i32 frames = 0;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<Spawner>("Spawner").field("count", &Spawner::count);
    types.add<Child>("Child").field("number", &Child::number).field("label", &Child::label);
    types.add<Lifetime>("Lifetime").field("frames", &Lifetime::frames);
    return types;
}

// Liste aller Child-Komponenten in der Reihenfolge der Kennungen: zeigt, in welcher Reihenfolge
// die Befehle die Entities angelegt haben.
std::vector<std::string> children_in_id_order(world::World& world) {
    std::vector<std::string> result;
    for (const u64 id : world.entity_ids()) {
        if (const auto* child = world.entity(id).get<Child>()) {
            result.push_back(child->label);
        }
    }
    return result;
}

std::vector<std::string> spawn_and_collect(u32 workers) {
    const auto types = make_types();
    world::World world{types};
    for (i32 i = 0; i < 2000; ++i) {
        world.create().set(Spawner{.count = 1 + i % 3});
    }
    jobs::JobSystem jobs{{.worker_count = workers}};
    world::Schedule schedule{world};
    schedule.add<const Spawner>(
        "Anlegen",
        [](world::Commands& commands, world::Entity entity, const Spawner& spawner) {
            for (i32 i = 0; i < spawner.count; ++i) {
                world::Entity child = commands.create();
                commands.set(child,
                             Child{.number = i,
                                   .label = std::to_string(entity.id()) + "/" + std::to_string(i)});
                commands.set_parent(child, entity);
            }
        },
        {.grain = 64});
    schedule.run(jobs);
    return children_in_id_order(world);
}

} // namespace

TEST_CASE("Commands create entities with pending handles", "[world][commands]") {
    const auto types = make_types();
    world::World world{types};
    world::Commands commands{world};

    world::Entity parent = commands.create("Eltern");
    world::Entity child = commands.create();
    CHECK(world::Commands::is_pending(parent.id()));
    commands.set(child, Child{.number = 7, .label = "ein langer Text, der nicht in SSO passt"});
    commands.set_parent(child, parent);
    CHECK(commands.size() == 4);
    CHECK(world.entity_count() == 0);

    commands.apply();
    CHECK(commands.empty());
    REQUIRE(world.entity_count() == 2);
    world::Entity real_parent;
    for (const u64 id : world.entity_ids()) {
        if (world.entity(id).name() == "Eltern") {
            real_parent = world.entity(id);
        }
    }
    REQUIRE(real_parent);
    const auto children = real_parent.children();
    REQUIRE(children.size() == 1);
    const Child* value = children[0].get<Child>();
    REQUIRE(value != nullptr);
    CHECK(value->number == 7);
    CHECK(value->label == "ein langer Text, der nicht in SSO passt");
}

TEST_CASE("Commands destroy, remove and skip dead entities", "[world][commands]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity a = world.create();
    a.set(Lifetime{.frames = 1}).set(Child{.number = 1, .label = "a"});
    world::Entity b = world.create();

    world::Commands commands{world};
    commands.remove<Child>(a);
    commands.destroy(b);
    // b ist dann schon zerstört: Der Befehl entfällt ohne Fehler.
    commands.set(b, Lifetime{.frames = 5});
    commands.apply();

    CHECK(a.alive());
    CHECK_FALSE(a.has<Child>());
    CHECK(a.has<Lifetime>());
    CHECK_FALSE(b.alive());

    // Verworfene Befehle geben ihre Werte frei (unter ASan/Valgrind sichtbar) und ändern nichts.
    commands.set(a, Child{.number = 2, .label = std::string(100, 'x')});
    commands.clear();
    CHECK_FALSE(a.has<Child>());
}

TEST_CASE("Commands instantiate prefabs", "[world][commands]") {
    const auto types = make_types();
    world::World source{types};
    world::Entity root = source.create("Kiste");
    root.set(world::Transform{});
    source.create("Deckel").set_parent(root);
    const world::Prefab prefab = world::make_prefab(source, root.id());

    world::World world{types};
    world::Commands commands{world};
    world::Entity instance = commands.instantiate(prefab, 42);
    commands.set(instance, Lifetime{.frames = 3});
    commands.apply();
    REQUIRE(world.entity_count() == 2);
    usize with_lifetime = 0;
    world.each<const Lifetime>([&](world::Entity entity, const Lifetime& lifetime) {
        CHECK(lifetime.frames == 3);
        CHECK(entity.name() == "Kiste");
        CHECK(entity.children().size() == 1);
        ++with_lifetime;
    });
    CHECK(with_lifetime == 1);
}

TEST_CASE("Systems change structure through commands", "[world][commands]") {
    const auto types = make_types();
    world::World world{types};
    for (i32 i = 0; i < 100; ++i) {
        world.create().set(Lifetime{.frames = i % 4});
    }
    jobs::JobSystem jobs{{.worker_count = 3}};
    world::Schedule schedule{world};
    schedule.add<Lifetime>("Altern",
                           [](world::Commands& commands, world::Entity entity, Lifetime& lifetime) {
                               if (lifetime.frames == 0) {
                                   commands.destroy(entity);
                               } else {
                                   --lifetime.frames;
                               }
                           },
                           {.grain = 8});

    CHECK(world.entity_count() == 100);
    schedule.run(jobs);
    CHECK(world.entity_count() == 75);
    schedule.run(jobs);
    schedule.run(jobs);
    schedule.run(jobs);
    CHECK(world.entity_count() == 0);
}

TEST_CASE("Command order is the same with 1 and 16 threads", "[world][commands]") {
    const auto single = spawn_and_collect(1);
    const auto many = spawn_and_collect(16);
    CHECK(single.size() == 2000 + 2000 / 3 * 3 + 1);
    CHECK(single == many);
}
