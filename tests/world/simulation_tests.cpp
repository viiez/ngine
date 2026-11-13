#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstring>
#include <string>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.jobs;
import ngine.world;

using namespace ngine;
using Catch::Matchers::WithinAbs;

namespace {

struct Velocity {
    f64 x = 0.0;
};
struct Counter {
    u64 fixed = 0;
    u64 frame = 0;
};
struct Log {
    std::string order;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<Velocity>("Velocity").field("x", &Velocity::x);
    types.add<Counter>("Counter").field("fixed", &Counter::fixed).field("frame", &Counter::frame);
    types.add<Log>("Log").field("order", &Log::order);
    return types;
}

struct Outcome {
    u64 ticks = 0;
    std::vector<world::Transform> transforms;
    std::vector<Velocity> velocities;
};

// Rechnet seconds Sekunden mit fps Bildern pro Sekunde. Die Bewegung hängt von der Lage ab
// (Feder), damit Unterschiede in der Schrittfolge sichtbar würden.
Outcome simulate(f64 fps, f64 seconds) {
    const auto types = make_types();
    world::World world{types};
    for (i32 i = 0; i < 50; ++i) {
        world.create()
            .set(world::Transform{.position = {static_cast<f64>(i), 0.0, 0.0}})
            .set(Velocity{.x = 1.0 + static_cast<f64>(i) * 0.1});
    }
    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Simulation simulation{world, jobs};
    simulation.add<Velocity, world::Transform>(
        "Feder",
        [&world](Velocity& velocity, world::Transform& transform) {
            const f64 dt = world.time().delta;
            velocity.x -= transform.position.x * 0.5 * dt;
            transform.position.x += velocity.x * dt;
            transform.rotation = math::normalize(
                transform.rotation * math::angle_axis(static_cast<f32>(dt), {0.0f, 1.0f, 0.0f}));
        },
        {.phase = world::Phase::Fixed});
    const auto frames = static_cast<i32>(fps * seconds + 0.5);
    for (i32 i = 0; i < frames; ++i) {
        simulation.advance(1.0 / fps);
    }
    Outcome outcome;
    outcome.ticks = simulation.time().tick;
    world.each<const world::Transform, const Velocity>(
        [&](const world::Transform& transform, const Velocity& velocity) {
            outcome.transforms.push_back(transform);
            outcome.velocities.push_back(velocity);
        });
    return outcome;
}

bool bit_equal(const Outcome& a, const Outcome& b) {
    return a.transforms.size() == b.transforms.size() &&
           std::memcmp(a.transforms.data(), b.transforms.data(),
                       a.transforms.size() * sizeof(world::Transform)) == 0 &&
           std::memcmp(a.velocities.data(), b.velocities.data(),
                       a.velocities.size() * sizeof(Velocity)) == 0;
}

} // namespace

TEST_CASE("Phases run separately and after orders systems", "[world][schedule]") {
    const auto types = make_types();
    world::World world{types};
    world.create().set(Log{}).set(Counter{});
    jobs::JobSystem jobs{{.worker_count = 3}};
    world::Schedule schedule{world};
    schedule.add<Log>("Fest", [](Log& log) { log.order += "F"; }, {.phase = world::Phase::Fixed});
    schedule.add<Log>("Bild", [](Log& log) { log.order += "B"; });
    schedule.add<Log>("Spaet", [](Log& log) { log.order += "S"; }, {.phase = world::Phase::Late});
    // Ohne gemeinsame Komponente liefen die beiden parallel; after erzwingt die Reihenfolge.
    schedule.add<Counter>("Zaehlen", [](Counter& counter) { ++counter.frame; });
    schedule.add<const Log>("Lesen", [](const Log&) {}, {.after = "Zaehlen"});

    CHECK(schedule.waits_for(4, 3));
    CHECK(schedule.waits_for(4, 1));       // liest Log, das Bild schreibt
    CHECK_FALSE(schedule.waits_for(1, 0)); // andere Phase

    schedule.run(jobs, world::Phase::Late);
    schedule.run(jobs, world::Phase::Fixed);
    schedule.run(jobs, world::Phase::Fixed);
    schedule.run(jobs);
    world.each<const Log, const Counter>([](const Log& log, const Counter& counter) {
        CHECK(log.order == "SFFB");
        CHECK(counter.frame == 1);
    });
}

TEST_CASE("Simulation steps fixed time independent of frame rate", "[world][simulation]") {
    const Outcome slow = simulate(30.0, 2.0);
    const Outcome fast = simulate(240.0, 2.0);
    const Outcome odd = simulate(144.0, 2.0);
    CHECK(slow.ticks == 120);
    CHECK(fast.ticks == 120);
    CHECK(odd.ticks == 120);
    CHECK(bit_equal(slow, fast));
    CHECK(bit_equal(slow, odd));
}

TEST_CASE("Simulation runs phases with their time", "[world][simulation]") {
    const auto types = make_types();
    world::World world{types};
    world.create().set(Counter{});
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Simulation simulation{world, jobs};
    std::vector<f32> fixed_deltas;
    std::vector<f32> frame_deltas;
    simulation.add<Counter>("Fest",
                            [&](Counter& counter) {
                                ++counter.fixed;
                                fixed_deltas.push_back(world.time().delta);
                            },
                            {.phase = world::Phase::Fixed});
    simulation.add<Counter>("Bild", [&](Counter& counter) {
        ++counter.frame;
        frame_deltas.push_back(world.time().delta);
    });
    u32 after_steps = 0;
    simulation.set_after_fixed_step([&] { ++after_steps; });

    // 25 ms: ein Schritt, 8,3 ms bleiben übrig.
    CHECK(simulation.advance(0.025) == 1);
    CHECK_THAT(simulation.time().alpha, WithinAbs(0.5, 1e-4));
    // Ein langes Bild rechnet höchstens vier Schritte; der Rest verfällt.
    CHECK(simulation.advance(1.0) == 4);
    CHECK(simulation.time().tick == 5);
    CHECK(after_steps == 5);
    CHECK_THAT(simulation.time().elapsed, WithinAbs(5.0 / 60.0, 1e-12));

    // Pause: keine Schritte, Frame läuft mit delta 0, Einzelschritt rechnet genau einen.
    simulation.set_paused(true);
    CHECK(simulation.advance(0.1) == 0);
    CHECK(simulation.time().alpha == 1.0f);
    simulation.step();
    CHECK(simulation.advance(0.1) == 1);
    CHECK(simulation.advance(0.1) == 0);
    simulation.set_paused(false);

    // Zeitlupe: halbe Geschwindigkeit, also ein Schritt aus zwei Bildern zu 1/60 s.
    simulation.set_time_scale(0.5);
    const u64 before = simulation.time().tick;
    for (int i = 0; i < 10; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    CHECK(simulation.time().tick - before == 6); // einer sofort nach der Pause, dann fünf

    for (const f32 delta : fixed_deltas) {
        CHECK(delta == 1.0f / 60.0f);
    }
    CHECK(frame_deltas.at(0) == 0.025f);
    CHECK(frame_deltas.at(2) == 0.0f); // in der Pause
    world.each<const Counter>([&](const Counter& counter) {
        CHECK(counter.fixed == simulation.time().tick);
        CHECK(counter.frame == 15);
    });
}

TEST_CASE("Simulation interpolates only Interpolated entities", "[world][simulation]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity smooth = world.create("Weich");
    smooth.set(world::Transform{}).set(Velocity{.x = 6.0}).set(world::Interpolated{});
    world::Entity child = world.create("Kind");
    child.set(world::Transform{.position = {0.0, 1.0, 0.0}}).set_parent(smooth);
    world::Entity jumpy = world.create("Sprung");
    jumpy.set(world::Transform{}).set(Velocity{.x = 6.0});
    world::Entity still = world.create("Ruhig");
    still.set(world::Transform{.position = {5.0, 0.0, 0.0}}).set(world::Interpolated{});

    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Simulation simulation{world, jobs};
    simulation.add<const Velocity, world::Transform>(
        "Bewegen",
        [&world](const Velocity& velocity, world::Transform& transform) {
            transform.position.x += velocity.x * world.time().delta;
        },
        {.phase = world::Phase::Fixed});

    // Erstes Bild: ein Schritt (0,1 m), alpha 0,5.
    CHECK(simulation.advance(1.5 / 60.0) == 1);
    CHECK_THAT(smooth.get<world::Transform>()->position.x, WithinAbs(0.1, 1e-6));
    // Gezeichnet wird zwischen 0 und 0,1 m, das Kind zieht mit.
    CHECK_THAT(smooth.get<world::WorldTransform>()->position().x, WithinAbs(0.05, 1e-6));
    CHECK_THAT(child.get<world::WorldTransform>()->position().x, WithinAbs(0.05, 1e-6));
    CHECK_THAT(child.get<world::WorldTransform>()->position().y, WithinAbs(1.0, 1e-6));
    // Ein weiterer Aufruf von update_transforms() (wie im Renderer) lässt die Mischung stehen.
    (void)world::update_transforms(world);
    CHECK_THAT(smooth.get<world::WorldTransform>()->position().x, WithinAbs(0.05, 1e-6));
    // Ohne Interpolated springt die Entity mit dem Schritt.
    CHECK_THAT(jumpy.get<world::WorldTransform>()->position().x, WithinAbs(0.1, 1e-6));
    CHECK_THAT(still.get<world::WorldTransform>()->position().x, WithinAbs(5.0, 1e-12));

    // Ein Bild ohne Schritt: alpha wächst, die Lage der Simulation bleibt.
    CHECK(simulation.advance(0.25 / 60.0) == 0);
    CHECK_THAT(smooth.get<world::Transform>()->position.x, WithinAbs(0.1, 1e-6));
    CHECK_THAT(smooth.get<world::WorldTransform>()->position().x, WithinAbs(0.075, 1e-6));
    CHECK_THAT(child.get<world::WorldTransform>()->position().x, WithinAbs(0.075, 1e-6));

    // Ruhende interpolierte Entities lösen keine Neuberechnung aus: Nach dem Anhalten und einem
    // Bild zum Nachziehen rechnet update_transforms() nichts mehr.
    smooth.remove<Velocity>();
    jumpy.remove<Velocity>();
    simulation.advance(1.0 / 60.0);
    simulation.advance(1.0 / 60.0);
    simulation.advance(1.0 / 60.0);
    CHECK_THAT(smooth.get<world::WorldTransform>()->position().x,
               WithinAbs(smooth.get<world::Transform>()->position.x, 1e-12));
    CHECK(world::update_transforms(world) == 0);

    // Die Szene speichert die Lage der Simulation, nicht die gemischte.
    const std::string saved = world::save_scene(world);
    CHECK(saved.find("Interpolated") != std::string::npos);
}
