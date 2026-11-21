#include <catch2/catch_test_macros.hpp>

import ngine.core;
import ngine.input;
import ngine.math;
import ngine.physics;
import ngine.platform;
import ngine.world;
import app.game_world;

using namespace ngine;

namespace {

// Boden als unbewegte Box, damit die Figuren ohne Terrain stehen.
void add_ground(world::World& world) {
    world.create("Boden")
        .set(world::Transform{.position = {0.0, -0.5, 0.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{.shape = physics::ShapeType::Box,
                               .half_extents = {30.0f, 0.5f, 30.0f}});
}

struct Run {
    f64 walked = 0.0;
    f64 stood = 0.0;
    u64 checksum = 0;
};

// Zwei Figuren mit eigenen Aktionen (wie zwei Spieler auf dem Server): Eine hält W, die andere
// nichts. Ohne Skripte steuert GameWorld::steer_players() sie in C++.
Run run_two_players(u32 frames) {
    app::GameWorld game{{.worker_count = 2, .deterministic = true}};
    add_ground(game.world());
    world::Entity walker = game.spawn_figure({-2.0, 0.0, 0.0}, "Läufer");
    world::Entity stander = game.spawn_figure({2.0, 0.0, 0.0}, "Steher");
    input::ActionState walk{input::ActionMap::standard()};
    input::ActionState idle{input::ActionMap::standard()};
    game.bind_player(walker, walk);
    game.bind_player(stander, idle);
    for (u32 frame = 0; frame < frames; ++frame) {
        walk.begin_frame();
        if (frame == 0) {
            walk.handle(platform::event::KeyPressed{platform::Key::W});
        }
        walk.end_frame(1.0f / 60.0f);
        idle.begin_frame();
        idle.end_frame(1.0f / 60.0f);
        game.steer_players();
        (void)game.update(1.0 / 60.0);
    }
    Run run;
    run.walked =
        math::length(walker.get<world::Transform>()->position - math::DVec3{-2.0, 0.0, 0.0});
    run.stood =
        math::length(stander.get<world::Transform>()->position - math::DVec3{2.0, 0.0, 0.0});
    run.checksum = physics::checksum(game.world());
    game.unbind_player(walker);
    game.unbind_player(stander);
    return run;
}

} // namespace

TEST_CASE("Game world steers each figure with its own actions", "[game]") {
    const Run run = run_two_players(90);
    // 1,5 s mit 4 m/s, abzüglich Anlauf.
    CHECK(run.walked > 3.0);
    CHECK(run.stood < 0.1);
}

TEST_CASE("Game world computes the same result twice", "[game]") {
    // Grundlage für den Vergleich von Server und --headless (gleiche Prüfsumme).
    const Run a = run_two_players(60);
    const Run b = run_two_players(60);
    CHECK(a.checksum == b.checksum);
}

TEST_CASE("Orbit moves along its path in fixed steps", "[game]") {
    app::GameWorld game{{.worker_count = 1}};
    const app::Orbit orbit{.radius_x = 2.0f, .radius_z = 2.0f, .speed = 1.0f};
    world::Entity moon = game.world().create("Mond");
    moon.set(world::Transform{.position = app::orbit_position(orbit)}).set(orbit);
    for (int i = 0; i < 60; ++i) {
        (void)game.update(1.0 / 60.0);
    }
    const auto* moved = moon.get<app::Orbit>();
    REQUIRE(moved != nullptr);
    CHECK(moved->time > 0.9f);
    const math::DVec3 at = moon.get<world::Transform>()->position;
    CHECK(math::length(at - app::orbit_position(*moved)) < 1e-6);
}
