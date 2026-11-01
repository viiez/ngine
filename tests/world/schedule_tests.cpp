#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

import ngine.core;
import ngine.reflect;
import ngine.jobs;
import ngine.world;

using namespace ngine;

namespace {

struct Position {
    f32 x = 0.0f;
};
struct Velocity {
    f32 x = 0.0f;
};
struct Health {
    i32 value = 100;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<Position>("Position").field("x", &Position::x);
    types.add<Velocity>("Velocity").field("x", &Velocity::x);
    types.add<Health>("Health").field("value", &Health::value);
    return types;
}

} // namespace

TEST_CASE("Schedule orders systems only when they conflict", "[world][schedule]") {
    const auto types = make_types();
    world::World world{types};
    world::Schedule schedule{world};

    schedule.add<const Velocity, Position>("Bewegen", [](const Velocity&, Position&) {});
    schedule.add<const Position>("Lesen A", [](const Position&) {});
    schedule.add<const Position>("Lesen B", [](const Position&) {});
    schedule.add<Health>("Heilen", [](Health&) {});
    schedule.add<Velocity>("Bremsen", [](Velocity&) {});

    CHECK(schedule.waits_for(1, 0)); // liest Position, die Bewegen schreibt
    CHECK(schedule.waits_for(2, 0));
    CHECK_FALSE(schedule.waits_for(2, 1)); // zwei Leser vertragen sich
    CHECK_FALSE(schedule.waits_for(3, 0)); // andere Komponente
    CHECK(schedule.waits_for(4, 0));       // schreibt Velocity, die Bewegen liest
    CHECK_FALSE(schedule.waits_for(4, 3));
}

TEST_CASE("Schedule runs systems in dependency order", "[world][schedule]") {
    const auto types = make_types();
    world::World world{types};
    for (int i = 0; i < 1000; ++i) {
        world::Entity entity = world.create();
        entity.set(Position{0.0f});
        entity.set(Velocity{1.0f});
        if (i % 2 == 0) {
            entity.set(Health{50});
        }
    }

    jobs::JobSystem jobs{{.worker_count = 3}};
    world::Schedule schedule{world};
    schedule.add<const Velocity, Position>(
        "Bewegen", [](const Velocity& v, Position& p) { p.x += v.x; }, {.grain = 64});
    // Muss nach Bewegen laufen und sieht deshalb schon die neue Position.
    schedule.add<const Position, Velocity>(
        "Bremsen", [](const Position& p, Velocity& v) { v.x = p.x >= 1.0f ? 0.5f : 99.0f; });
    schedule.add<Health>("Heilen", [](Health& h) { h.value += 1; }, {.grain = 16});

    for (int frame = 0; frame < 3; ++frame) {
        schedule.run(jobs);
    }

    // Frame 1: x = 1, v = 0.5; Frame 2: x = 1.5; Frame 3: x = 2.
    world.each<const Position, const Velocity>([](const Position& p, const Velocity& v) {
        CHECK(p.x == 2.0f);
        CHECK(v.x == 0.5f);
    });
    int healed = 0;
    world.each<const Health>([&](const Health& h) {
        CHECK(h.value == 53);
        ++healed;
    });
    CHECK(healed == 500);
}

TEST_CASE("Independent systems run in parallel", "[world][schedule]") {
    const auto types = make_types();
    world::World world{types};
    world.create().set(Position{});
    world.create().set(Health{});

    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Schedule schedule{world};
    // Beide Systeme warten aufeinander. Laufen sie nicht gleichzeitig, kommt keines weiter
    // (nach einer Sekunde gibt der Test auf, statt zu hängen).
    std::atomic<int> arrived{0};
    std::atomic<bool> both{false};
    auto meet = [&] {
        arrived.fetch_add(1);
        for (int i = 0; i < 1000 && arrived.load() < 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        if (arrived.load() == 2) {
            both.store(true);
        }
    };
    schedule.add<Position>("A", [&](Position&) { meet(); });
    schedule.add<Health>("B", [&](Health&) { meet(); });
    schedule.run(jobs);
    CHECK(both.load());
}

TEST_CASE("Schedule works without worker threads", "[world][schedule]") {
    const auto types = make_types();
    world::World world{types};
    for (int i = 0; i < 100; ++i) {
        world.create().set(Position{});
    }
    jobs::JobSystem jobs{{.worker_count = 0}};
    world::Schedule schedule{world};
    schedule.add<Position>("Zählen", [](Position& p) { p.x += 1.0f; }, {.grain = 8});
    schedule.run(jobs);
    world.each<const Position>([](const Position& p) { CHECK(p.x == 1.0f); });
}
