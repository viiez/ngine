#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.jobs;
import ngine.world;
import ngine.physics;
import ngine.edit;

using namespace ngine;
using Catch::Matchers::WithinAbs;

namespace {

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    physics::register_types(types);
    return types;
}

world::Entity add_floor(world::World& world, math::DVec3 origin) {
    world::Entity floor = world.create("Boden");
    floor.set(world::Transform{.position = origin + math::DVec3{0.0, -0.5, 0.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{.shape = physics::ShapeType::Box,
                               .half_extents = {50.0f, 0.5f, 50.0f}});
    return floor;
}

// Stapel aus count Kisten zu 1 m, jede um shift versetzt (ab 0,1 m fällt er um).
std::vector<world::Entity> add_stack(world::World& world, math::DVec3 origin, i32 count,
                                     f64 shift = 0.12) {
    std::vector<world::Entity> boxes;
    for (i32 i = 0; i < count; ++i) {
        world::Entity box = world.create();
        box.set(world::Transform{.position = origin + math::DVec3{shift * i, 0.5 + 1.01 * i, 0.0}})
            .set(physics::RigidBody{})
            .set(physics::Collider{.shape = physics::ShapeType::Box});
        boxes.push_back(box);
    }
    return boxes;
}

struct Run {
    u64 checksum = 0;
    std::vector<math::DVec3> positions;
};

// Rechnet die Szene aus Boden und Stapel seconds Sekunden lang.
Run simulate_stack(u32 workers, math::DVec3 origin, f64 seconds, f64 shift = 0.12) {
    const auto types = make_types();
    world::World world{types};
    add_floor(world, origin);
    const auto boxes = add_stack(world, origin, 12, shift);
    jobs::JobSystem jobs{{.worker_count = workers}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    physics::add_to(simulation, physics);
    const auto frames = static_cast<i32>(seconds * 60.0 + 0.5);
    for (i32 i = 0; i < frames; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    Run run;
    run.checksum = physics::checksum(world);
    for (const world::Entity& box : boxes) {
        run.positions.push_back(box.get<world::Transform>()->position - origin);
    }
    return run;
}

} // namespace

TEST_CASE("Body falls and comes to rest on the floor", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    add_floor(world, {});
    world::Entity ball = world.create("Kugel");
    ball.set(world::Transform{.position = {0.0, 5.0, 0.0}})
        .set(physics::RigidBody{})
        .set(physics::Collider{.shape = physics::ShapeType::Sphere, .radius = 0.5f});
    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    physics::add_to(simulation, physics);

    simulation.advance(1.0 / 60.0);
    CHECK(physics.body_count() == 2);
    CHECK(ball.has<world::Interpolated>());
    // Nach einer Sechzigstelsekunde ist sie gefallen und hat Geschwindigkeit nach unten.
    CHECK(ball.get<world::Transform>()->position.y < 5.0);
    CHECK(ball.get<physics::RigidBody>()->linear_velocity.y < 0.0f);

    for (i32 i = 0; i < 240; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    CHECK_THAT(ball.get<world::Transform>()->position.y, WithinAbs(0.5, 0.03));
    CHECK_FALSE(physics.is_active(ball.id()));
    CHECK(ball.get<physics::RigidBody>()->linear_velocity.y == 0.0f);
}

TEST_CASE("Same scene gives bit-equal results with 1 and 16 threads", "[physics]") {
    const Run single = simulate_stack(1, {}, 5.0);
    const Run many = simulate_stack(16, {}, 5.0);
    CHECK(single.checksum == many.checksum);
    // Der Stapel ist tatsächlich umgefallen.
    CHECK(single.positions.back().y < 6.0);
}

TEST_CASE("Physics far from the origin matches the origin", "[physics]") {
    // Ein stehender Stapel: Ein fallender ist chaotisch, kleinste Rundungsunterschiede wachsen dort
    // zu Zentimetern.
    const Run near = simulate_stack(2, {}, 5.0, 0.02);
    const Run far = simulate_stack(2, {1.0e7, 0.0, -1.0e7}, 5.0, 0.02);
    REQUIRE(near.positions.size() == far.positions.size());
    for (usize i = 0; i < near.positions.size(); ++i) {
        CHECK(math::distance(near.positions[i], far.positions[i]) < 1.0e-3);
    }
}

TEST_CASE("Clearing and running again reproduces the run", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    add_floor(world, {});
    add_stack(world, {}, 8);
    const std::string saved = world::save_scene(world);
    jobs::JobSystem jobs{{.worker_count = 3}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    physics::add_to(simulation, physics);
    for (i32 i = 0; i < 120; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    const u64 first = physics::checksum(world);

    // Wie Stoppen im Spielmodus: Welt aus der Sicherung, Physik verworfen.
    physics.clear();
    for (const u64 id : world.entity_ids()) {
        world.destroy(id);
    }
    REQUIRE(world::load_scene(world, saved).has_value());
    simulation.reset_clock();
    for (i32 i = 0; i < 120; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    CHECK(physics::checksum(world) == first);
}

TEST_CASE("Queries find entities and respect filters", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity floor = add_floor(world, {});
    world::Entity near = world.create();
    near.set(world::Transform{.position = {0.0, 1.0, -5.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{.shape = physics::ShapeType::Box});
    world::Entity far = world.create();
    far.set(world::Transform{.position = {0.0, 1.0, -10.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Kinematic,
                                .layer = physics::PhysicsLayer::Debris})
        .set(physics::Collider{.shape = physics::ShapeType::Sphere});
    world::Entity sensor = world.create();
    sensor.set(world::Transform{.position = {0.0, 1.0, -2.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{.shape = physics::ShapeType::Box, .sensor = true});
    jobs::JobSystem jobs{{.worker_count = 1}};
    physics::PhysicsWorld physics{world, jobs};
    physics.sync();

    const math::DVec3 eye{0.0, 1.0, 0.0};
    const math::Vec3 forward{0.0f, 0.0f, -1.0f};
    const auto hit = physics.raycast(eye, forward, 100.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == near.id());
    CHECK_THAT(hit->distance, WithinAbs(4.5, 1e-4));
    CHECK_THAT(hit->point.z, WithinAbs(-4.5, 1e-4));
    CHECK_THAT(hit->normal.z, WithinAbs(1.0, 1e-4));

    // Ohne Ebene Static trifft der Strahl die Kugel dahinter; mit ignore ebenso.
    const auto debris = physics.raycast(
        eye, forward, 100.0f, {.layers = physics::layer_bit(physics::PhysicsLayer::Debris)});
    REQUIRE(debris.has_value());
    CHECK(debris->entity == far.id());
    const auto ignored = physics.raycast(eye, forward, 100.0f, {.ignore = near.id()});
    REQUIRE(ignored.has_value());
    CHECK(ignored->entity == far.id());

    // Sensoren nur auf Wunsch.
    const auto all = physics.raycast_all(eye, forward, 100.0f, {.sensors = true});
    REQUIRE(all.size() == 3);
    CHECK(all[0].entity == sensor.id());
    CHECK(all[1].entity == near.id());
    CHECK(all[2].entity == far.id());

    // Nach unten trifft der Strahl den Boden.
    const auto down = physics.raycast({3.0, 10.0, 3.0}, {0.0f, -1.0f, 0.0f}, 100.0f);
    REQUIRE(down.has_value());
    CHECK(down->entity == floor.id());
    CHECK_THAT(down->point.y, WithinAbs(0.0, 1e-4));

    // Eine Kugel mit 0,5 m Radius berührt die Kiste 0,5 m früher als der Strahl.
    const auto cast = physics.shape_cast({.shape = physics::ShapeType::Sphere, .radius = 0.5f}, eye,
                                         math::quat_identity(), forward, 100.0f);
    REQUIRE(cast.has_value());
    CHECK(cast->entity == near.id());
    CHECK_THAT(cast->distance, WithinAbs(4.0, 1e-3));
    CHECK_THAT(cast->point.z, WithinAbs(-4.5, 1e-3));

    const auto touched =
        physics.overlap({.shape = physics::ShapeType::Box, .half_extents = {1.0f, 1.0f, 4.0f}},
                        {0.0, 1.0, -5.0}, math::quat_identity(), {.sensors = true});
    CHECK(touched == std::vector<u64>{floor.id(), near.id(), sensor.id()});
    CHECK(physics
              .overlap({.shape = physics::ShapeType::Sphere, .radius = 0.2f}, {20.0, 5.0, 20.0},
                       math::quat_identity())
              .empty());
}

TEST_CASE("Contacts and triggers arrive as events in order", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity floor = add_floor(world, {});
    world::Entity sensor = world.create("Ausloeser");
    sensor.set(world::Transform{.position = {0.0, 2.0, 0.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{.shape = physics::ShapeType::Box, .sensor = true});
    world::Entity ball = world.create("Kugel");
    ball.set(world::Transform{.position = {0.0, 5.0, 0.0}})
        .set(physics::RigidBody{})
        .set(physics::Collider{.shape = physics::ShapeType::Sphere, .radius = 0.25f});

    events::EventBus bus;
    std::vector<std::string> log;
    auto began = bus.subscribe<physics::ContactBegan>([&](const physics::ContactBegan& event) {
        CHECK(event.a < event.b);
        CHECK(event.normal.y > 0.9f); // von a (Boden) nach b (Kugel)
        log.push_back(event.a == floor.id() && event.b == ball.id() ? "beruehrt" : "falsch");
    });
    auto entered =
        bus.subscribe<physics::TriggerEntered>([&](const physics::TriggerEntered& event) {
            log.push_back(event.sensor == sensor.id() && event.other == ball.id() ? "rein"
                                                                                  : "falsch");
        });
    auto left = bus.subscribe<physics::TriggerLeft>([&](const physics::TriggerLeft& event) {
        log.push_back(event.sensor == sensor.id() && event.other == ball.id() ? "raus" : "falsch");
    });
    auto ended = bus.subscribe<physics::ContactEnded>(
        [&](const physics::ContactEnded&) { log.push_back("los"); });

    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    physics.set_events(&bus);
    physics::add_to(simulation, physics);
    for (i32 i = 0; i < 180; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    CHECK(log == std::vector<std::string>{"rein", "raus", "beruehrt"});

    // Die Kugel verschwindet: Der Kontakt endet mit ihr.
    ball.destroy();
    simulation.advance(1.0 / 60.0);
    CHECK(log.back() == "los");
    CHECK(physics.body_count() == 2);
}

TEST_CASE("Bodies follow changes of components", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    add_floor(world, {});
    world::Entity box = world.create();
    box.set(world::Transform{.position = {0.0, 0.5, 0.0}})
        .set(physics::RigidBody{})
        .set(physics::Collider{.shape = physics::ShapeType::Box});
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    physics::add_to(simulation, physics);
    for (i32 i = 0; i < 60; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    CHECK_THAT(box.get<world::Transform>()->position.y, WithinAbs(0.5, 0.01));

    // Größere Form: Der Körper entsteht neu und steht höher.
    box.set(
        physics::Collider{.shape = physics::ShapeType::Box, .half_extents = {1.0f, 1.0f, 1.0f}});
    for (i32 i = 0; i < 120; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    CHECK_THAT(box.get<world::Transform>()->position.y, WithinAbs(1.0, 0.03));

    // Versetzen wie mit dem Gizmo: Der Körper fällt von dort.
    world::Transform moved = *box.get<world::Transform>();
    moved.position = {10.0, 6.0, 0.0};
    box.set(moved);
    simulation.advance(1.0 / 60.0);
    CHECK(box.get<world::Transform>()->position.x == 10.0);
    CHECK(box.get<world::Transform>()->position.y < 6.0);

    // Wurf: Geschwindigkeit in der Komponente.
    physics::RigidBody thrown = *box.get<physics::RigidBody>();
    thrown.linear_velocity = {5.0f, 0.0f, 0.0f};
    box.set(thrown);
    simulation.advance(1.0 / 60.0);
    CHECK(box.get<world::Transform>()->position.x > 10.05);

    box.remove<physics::Collider>();
    simulation.advance(1.0 / 60.0);
    CHECK_FALSE(physics.has_body(box.id()));
    CHECK(physics.body_count() == 1);
}

TEST_CASE("Kinematic bodies follow their transform and push others", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity pusher = world.create();
    pusher.set(world::Transform{.position = {0.0, 0.5, 0.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Kinematic})
        .set(physics::Collider{.shape = physics::ShapeType::Box});
    world::Entity box = world.create();
    box.set(world::Transform{.position = {1.2, 0.5, 0.0}})
        .set(physics::RigidBody{.gravity_factor = 0.0f})
        .set(physics::Collider{.shape = physics::ShapeType::Box,
                               .half_extents = {0.2f, 0.2f, 0.2f}});
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Simulation simulation{world, jobs};
    simulation.add<const physics::RigidBody, world::Transform>(
        "Schieben",
        [&world](const physics::RigidBody& body, world::Transform& transform) {
            if (body.motion == physics::MotionType::Kinematic) {
                transform.position.x += 2.0 * world.time().delta;
            }
        },
        {.phase = world::Phase::Fixed});
    physics::PhysicsWorld physics{world, jobs};
    physics::add_to(simulation, physics);
    for (i32 i = 0; i < 60; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    CHECK_THAT(pusher.get<world::Transform>()->position.x, WithinAbs(2.0, 1e-5));
    // Die kleine Kiste liegt jetzt vor dem Schieber.
    CHECK(box.get<world::Transform>()->position.x > 2.6);
}

TEST_CASE("Mesh and hull shapes come from the shape source", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity ground = world.create("Rampe");
    ground.set(world::Transform{.scale = {10.0f, 1.0f, 10.0f}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{.shape = physics::ShapeType::Mesh, .model = 7, .mesh = 1});
    world::Entity rock = world.create("Fels");
    rock.set(world::Transform{.position = {0.0, 3.0, 0.0}})
        .set(physics::RigidBody{})
        .set(physics::Collider{.shape = physics::ShapeType::ConvexHull, .model = 7, .mesh = 2});
    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    physics::add_to(simulation, physics);

    // Erst ohne Quelle: Keine Körper, keine Abstürze.
    simulation.advance(1.0 / 60.0);
    CHECK(physics.body_count() == 0);

    u32 requests = 0;
    physics.set_shape_source([&](u64 model, u32 mesh) -> std::optional<physics::TriangleMesh> {
        ++requests;
        if (model != 7) {
            return std::nullopt;
        }
        if (mesh == 1) {
            // Quadrat in der Ebene y = 0, Normale nach oben.
            return physics::TriangleMesh{.positions = {{-1.0f, 0.0f, -1.0f},
                                                       {1.0f, 0.0f, -1.0f},
                                                       {1.0f, 0.0f, 1.0f},
                                                       {-1.0f, 0.0f, 1.0f}},
                                         .indices = {0, 2, 1, 0, 3, 2}};
        }
        // Würfel mit 0,5 m Kantenlänge als Punktwolke.
        physics::TriangleMesh cube;
        for (const f32 x : {-0.25f, 0.25f}) {
            for (const f32 y : {-0.25f, 0.25f}) {
                for (const f32 z : {-0.25f, 0.25f}) {
                    cube.positions.push_back({x, y, z});
                }
            }
        }
        return cube;
    });
    for (i32 i = 0; i < 180; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    CHECK(physics.body_count() == 2);
    CHECK(requests == 2); // gebaut einmal, danach aus dem Cache
    CHECK_THAT(rock.get<world::Transform>()->position.y, WithinAbs(0.25, 0.03));
}

TEST_CASE("Dynamic child writes back its local transform", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    add_floor(world, {});
    world::Entity parent = world.create("Gruppe");
    parent.set(world::Transform{.position = {5.0, 0.0, 0.0}});
    world::Entity child = world.create("Kiste");
    child.set(world::Transform{.position = {0.0, 3.0, 0.0}})
        .set(physics::RigidBody{})
        .set(physics::Collider{.shape = physics::ShapeType::Box})
        .set_parent(parent);
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    physics::add_to(simulation, physics);
    for (i32 i = 0; i < 180; ++i) {
        simulation.advance(1.0 / 60.0);
    }
    CHECK_THAT(child.get<world::Transform>()->position.x, WithinAbs(0.0, 1e-3));
    CHECK_THAT(child.get<world::Transform>()->position.y, WithinAbs(0.5, 0.03));
    CHECK_THAT(child.get<world::WorldTransform>()->position().x, WithinAbs(5.0, 1e-3));
}

TEST_CASE("Physics components round-trip through scenes", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity box = world.create("Kiste");
    box.set(world::Transform{})
        .set(physics::RigidBody{.motion = physics::MotionType::Kinematic,
                                .layer = physics::PhysicsLayer::Debris,
                                .mass = 12.0f,
                                .linear_velocity = {1.0f, 2.0f, 3.0f}})
        .set(physics::Collider{.shape = physics::ShapeType::Capsule, .radius = 0.3f, .model = 99});
    const std::string saved = world::save_scene(world);
    world::World loaded{types};
    REQUIRE(world::load_scene(loaded, saved).has_value());
    usize count = 0;
    loaded.each<const physics::RigidBody, const physics::Collider>(
        [&](const physics::RigidBody& body, const physics::Collider& collider) {
            CHECK(body.motion == physics::MotionType::Kinematic);
            CHECK(body.layer == physics::PhysicsLayer::Debris);
            CHECK(body.mass == 12.0f);
            CHECK(body.linear_velocity.z == 3.0f);
            CHECK(collider.shape == physics::ShapeType::Capsule);
            CHECK(collider.radius == 0.3f);
            CHECK(collider.model == 99);
            ++count;
        });
    CHECK(count == 1);
}

TEST_CASE("Layer table is symmetric", "[physics]") {
    for (u32 a = 0; a < physics::layer_count; ++a) {
        for (u32 b = 0; b < physics::layer_count; ++b) {
            CHECK(physics::layers_collide(static_cast<physics::PhysicsLayer>(a),
                                          static_cast<physics::PhysicsLayer>(b)) ==
                  physics::layers_collide(static_cast<physics::PhysicsLayer>(b),
                                          static_cast<physics::PhysicsLayer>(a)));
        }
    }
    CHECK_FALSE(
        physics::layers_collide(physics::PhysicsLayer::Debris, physics::PhysicsLayer::Moving));
    CHECK(physics::layers_collide(physics::PhysicsLayer::Debris, physics::PhysicsLayer::Static));
    CHECK_FALSE(
        physics::layers_collide(physics::PhysicsLayer::Sensor, physics::PhysicsLayer::Static));
}

TEST_CASE("Play in the editor topples a stack and stop restores it bit for bit", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    events::EventBus bus;
    edit::SceneDocument document{world, &bus};
    add_floor(world, {});
    add_stack(world, {}, 8);
    (void)world::update_transforms(world);
    const std::vector<std::byte> before = world::save_scene_binary(world);

    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    physics::add_to(simulation, physics);
    const auto play = [&] {
        REQUIRE(document.begin_play());
        for (i32 i = 0; i < 180; ++i) {
            simulation.advance(1.0 / 60.0);
        }
        const u64 sum = physics::checksum(world);
        CHECK(world::save_scene_binary(world) != before);
        REQUIRE(document.end_play());
        // Wie im Editor: Uhr zurück, Körper verwerfen.
        simulation.reset_clock();
        physics.clear();
        return sum;
    };
    const u64 first = play();
    CHECK(world::save_scene_binary(world) == before);
    // Noch einmal spielen ergibt dasselbe.
    CHECK(play() == first);
    CHECK(world::save_scene_binary(world) == before);
}

// --- Kollision des Terrains (R4.3) ------------------------------------------------------------

namespace {

// Ebenes Terrain fester Größe (2 × 1 × 2 Chunks, also 32 m × 16 m × 32 m) mit Oberfläche bei 8 m.
world::Entity add_flat_terrain(world::World& world, math::DVec3 origin) {
    world::Entity terrain = world.create("Terrain");
    terrain.set(world::Transform{.position = origin})
        .set(world::Terrain{.size_x = 2,
                            .size_y = 1,
                            .size_z = 2,
                            .shape = world::TerrainShape::Flat,
                            .ground = 8.0f});
    return terrain;
}

world::Entity add_box(world::World& world, math::DVec3 position) {
    world::Entity box = world.create();
    box.set(world::Transform{.position = position})
        .set(physics::RigidBody{})
        .set(physics::Collider{.shape = physics::ShapeType::Box});
    return box;
}

// Ein Bild: Terrain aktuell halten, dann die Simulation.
void frame(world::World& world, jobs::JobSystem& jobs, world::Simulation& simulation) {
    world::update_transforms(world);
    (void)world::update_terrains(world, jobs);
    simulation.advance(1.0 / 60.0);
}

u64 simulate_on_terrain(u32 workers) {
    const auto types = make_types();
    world::World world{types};
    add_flat_terrain(world, {});
    for (i32 i = 0; i < 6; ++i) {
        add_box(world, {0.3 * i, 10.0 + 1.1 * i, 0.2 * i});
    }
    jobs::JobSystem jobs{{.worker_count = workers}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs, {.wait_for_terrain = true}};
    physics::add_to(simulation, physics);
    for (i32 i = 0; i < 180; ++i) {
        frame(world, jobs, simulation);
    }
    return physics::checksum(world);
}

} // namespace

TEST_CASE("Bodies land on terrain collision built from chunk meshes", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    const world::Entity terrain = add_flat_terrain(world, {});
    const world::Entity box = add_box(world, {1.0, 12.0, 1.0});
    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs, {.wait_for_terrain = true}};
    physics::add_to(simulation, physics);

    for (i32 i = 0; i < 180; ++i) {
        frame(world, jobs, simulation);
    }
    CHECK_THAT(box.get<world::Transform>()->position.y, WithinAbs(8.5, 0.05));
    CHECK_FALSE(physics.is_frozen(box.id()));
    // Nur die Chunks in der Nähe der Kiste haben eine Form, und ein Strahl trifft das Terrain.
    const std::vector<physics::TerrainChunkShape> shapes = physics.terrain_shapes();
    CHECK_FALSE(shapes.empty());
    CHECK(shapes.size() < 16);
    CHECK(physics.stats().terrain_bytes > 0);
    const auto hit = physics.raycast({-5.0, 12.0, -5.0}, {0.0f, -1.0f, 0.0f}, 10.0f);
    REQUIRE(hit);
    CHECK(hit->entity == terrain.id());
    CHECK_THAT(hit->point.y, WithinAbs(8.0, 0.05));
}

TEST_CASE("Digging under a box lets it fall", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    world::Entity terrain = add_flat_terrain(world, {});
    const world::Entity box = add_box(world, {2.0, 12.0, 2.0});
    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs, {.wait_for_terrain = true}};
    physics::add_to(simulation, physics);
    for (i32 i = 0; i < 180; ++i) {
        frame(world, jobs, simulation);
    }
    REQUIRE_THAT(box.get<world::Transform>()->position.y, WithinAbs(8.5, 0.05));
    REQUIRE_FALSE(physics.is_active(box.id()));

    // Eine Grube von 3 m Radius unter der Kiste: Die Form wird ersetzt, die Kiste wacht auf.
    const math::DVec3 center = world::terrain_voxels(terrain, {2.0, 8.0, 2.0});
    const voxel::Brush brush{.mode = voxel::BrushMode::Remove,
                             .center = {static_cast<f32>(center.x), static_cast<f32>(center.y),
                                        static_cast<f32>(center.z)},
                             .radius = 6.0f};
    (void)world::edit_terrain(terrain, brush);
    for (i32 i = 0; i < 120; ++i) {
        frame(world, jobs, simulation);
    }
    CHECK(box.get<world::Transform>()->position.y < 7.0);
}

TEST_CASE("Bodies stay frozen until the terrain below has collision", "[physics]") {
    const auto types = make_types();
    world::World world{types};
    add_flat_terrain(world, {});
    const world::Entity box = add_box(world, {1.0, 12.0, 1.0});
    // Die Form entsteht in einem Job; ist sie im ersten Schritt noch nicht fertig, bleibt die
    // Kiste stehen, statt zu fallen.
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    physics::add_to(simulation, physics);

    frame(world, jobs, simulation);
    if (physics.stats().terrain_built == 0) {
        CHECK(physics.is_frozen(box.id()));
        CHECK(box.get<world::Transform>()->position.y == 12.0);
    }
    // Wie im Spiel: Bilder zu 16 ms, bis die Formen fertig sind (in Debug einige Millisekunden pro
    // Chunk), dann noch drei Sekunden zum Fallen.
    for (i32 i = 0; i < 600 && physics.is_frozen(box.id()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
        frame(world, jobs, simulation);
    }
    for (i32 i = 0; i < 180; ++i) {
        frame(world, jobs, simulation);
    }
    CHECK_FALSE(physics.is_frozen(box.id()));
    CHECK_THAT(box.get<world::Transform>()->position.y, WithinAbs(8.5, 0.05));
}

TEST_CASE("Terrain collision is bit-equal with 1 and 4 threads when waiting", "[physics]") {
    CHECK(simulate_on_terrain(1) == simulate_on_terrain(4));
}

// --- Spielfiguren (R4.3) ----------------------------------------------------------------------

namespace {

world::Entity add_character(world::World& world, math::DVec3 position,
                            physics::CharacterController controller = {}) {
    world::Entity character = world.create("Figur");
    character.set(world::Transform{.position = position}).set(controller);
    return character;
}

// Unbewegter Quader mit Unterkante, Mitte und Drehung um z.
world::Entity add_static_box(world::World& world, math::DVec3 center, math::Vec3 half,
                             f32 angle_z = 0.0f) {
    world::Entity box = world.create();
    box.set(world::Transform{.position = center,
                             .rotation = math::angle_axis(angle_z, {0.0f, 0.0f, 1.0f})})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{.shape = physics::ShapeType::Box, .half_extents = half});
    return box;
}

struct CharacterScene {
    reflect::TypeRegistry types = make_types();
    world::World world{types};
    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs};
    events::EventBus bus;

    CharacterScene() {
        physics::add_to(simulation, physics);
        physics.set_events(&bus);
    }
    void run(world::Entity character, math::Vec3 move, i32 frames, bool jump = false) {
        for (i32 i = 0; i < frames; ++i) {
            if (character.has<physics::CharacterMotion>()) {
                auto* motion = character.get_mut<physics::CharacterMotion>();
                motion->move = move;
                motion->jump = jump && i == 0;
            }
            simulation.advance(1.0 / 60.0);
        }
    }
    [[nodiscard]] static math::DVec3 position(world::Entity e) {
        return e.get<world::Transform>()->position;
    }
};

} // namespace

TEST_CASE("Character stands on the floor and walks", "[physics]") {
    CharacterScene scene;
    add_floor(scene.world, {});
    world::Entity character = add_character(scene.world, {0.0, 1.0, 0.0});
    scene.run(character, {}, 60);
    REQUIRE(character.has<physics::CharacterMotion>());
    CHECK(character.has<world::Interpolated>());
    CHECK(character.get<physics::CharacterMotion>()->ground == physics::GroundState::Ground);
    CHECK_THAT(CharacterScene::position(character).y, WithinAbs(0.0, 0.05));

    // Eine Sekunde vorwärts mit 4 m/s, nach einer Zehntelsekunde Anlauf.
    scene.run(character, {1.0f, 0.0f, 0.0f}, 60);
    CHECK_THAT(CharacterScene::position(character).x, WithinAbs(3.8, 0.25));
    CHECK_THAT(CharacterScene::position(character).y, WithinAbs(0.0, 0.05));
    // Ein Strahl trifft die Figur über ihren inneren Körper.
    const auto hit =
        scene.physics.raycast(CharacterScene::position(character) + math::DVec3{0.0, 5.0, 0.0},
                              {0.0f, -1.0f, 0.0f}, 10.0f);
    REQUIRE(hit);
    CHECK(hit->entity == character.id());
}

TEST_CASE("Character climbs gentle slopes and steps but not walls", "[physics]") {
    SECTION("gentle slope") {
        CharacterScene scene;
        add_floor(scene.world, {});
        // Rampe mit 25 Grad, beginnt bei x = 2.
        add_static_box(scene.world, {6.0, 0.0, 0.0}, {4.5f, 0.5f, 3.0f}, 0.4363f);
        world::Entity character = add_character(scene.world, {0.0, 0.1, 0.0});
        scene.run(character, {1.0f, 0.0f, 0.0f}, 150);
        CHECK(CharacterScene::position(character).y > 1.0);
    }
    SECTION("steep slope") {
        CharacterScene scene;
        add_floor(scene.world, {});
        // 65 Grad: zu steil.
        add_static_box(scene.world, {4.0, 0.0, 0.0}, {3.0f, 0.5f, 3.0f}, 1.1345f);
        world::Entity character = add_character(scene.world, {0.0, 0.1, 0.0});
        scene.run(character, {1.0f, 0.0f, 0.0f}, 150);
        CHECK(CharacterScene::position(character).y < 0.7);
    }
    SECTION("low step") {
        CharacterScene scene;
        add_floor(scene.world, {});
        add_static_box(scene.world, {4.0, 0.15, 0.0}, {2.0f, 0.15f, 2.0f});
        world::Entity character = add_character(scene.world, {0.0, 0.1, 0.0});
        scene.run(character, {1.0f, 0.0f, 0.0f}, 60);
        CHECK_THAT(CharacterScene::position(character).y, WithinAbs(0.3, 0.05));
    }
    SECTION("high step") {
        CharacterScene scene;
        add_floor(scene.world, {});
        add_static_box(scene.world, {4.0, 0.4, 0.0}, {2.0f, 0.4f, 2.0f});
        world::Entity character = add_character(scene.world, {0.0, 0.1, 0.0});
        scene.run(character, {1.0f, 0.0f, 0.0f}, 60);
        CHECK(CharacterScene::position(character).x < 1.8);
        CHECK(CharacterScene::position(character).y < 0.1);
    }
}

TEST_CASE("Character jumps and reports the landing", "[physics]") {
    CharacterScene scene;
    add_floor(scene.world, {});
    world::Entity character = add_character(scene.world, {0.0, 0.05, 0.0});
    scene.run(character, {}, 30);
    std::vector<physics::CharacterLanded> landings;
    auto subscription = scene.bus.subscribe<physics::CharacterLanded>(
        [&](const physics::CharacterLanded& event) { landings.push_back(event); });

    f64 top = 0.0;
    for (i32 i = 0; i < 120; ++i) {
        scene.run(character, {}, 1, i == 0);
        top = std::max(top, CharacterScene::position(character).y);
    }
    CHECK_THAT(top, WithinAbs(1.2, 0.08));
    REQUIRE(landings.size() == 1);
    CHECK(landings[0].entity == character.id());
    CHECK_THAT(landings[0].height, WithinAbs(1.2, 0.08));
    CHECK(landings[0].speed > 4.0f);
}

TEST_CASE("Character walking reports steps", "[physics]") {
    CharacterScene scene;
    add_floor(scene.world, {});
    world::Entity character = add_character(scene.world, {0.0, 0.05, 0.0});
    scene.run(character, {}, 30);
    usize steps = 0;
    auto subscription = scene.bus.subscribe<physics::CharacterStep>(
        [&](const physics::CharacterStep&) { ++steps; });
    // Rund 3,8 m in einer Sekunde, also fünf Schritte zu 0,7 m.
    scene.run(character, {0.0f, 0.0f, -1.0f}, 60);
    CHECK(steps >= 4);
    CHECK(steps <= 6);
}

TEST_CASE("Character pushes a box", "[physics]") {
    CharacterScene scene;
    add_floor(scene.world, {});
    world::Entity box = add_box(scene.world, {2.0, 0.5, 0.0});
    box.get_mut<physics::RigidBody>()->mass = 20.0f;
    world::Entity character = add_character(scene.world, {0.0, 0.05, 0.0});
    scene.run(character, {1.0f, 0.0f, 0.0f}, 120);
    CHECK(box.get<world::Transform>()->position.x > 2.5);
}

TEST_CASE("Character crouches and stands up only with room above", "[physics]") {
    CharacterScene scene;
    add_floor(scene.world, {});
    // Decke in 1,4 m Höhe ab x = 2.
    add_static_box(scene.world, {5.0, 1.65, 0.0}, {3.0f, 0.25f, 3.0f});
    world::Entity character = add_character(scene.world, {0.0, 0.05, 0.0});
    scene.run(character, {}, 10);
    character.get_mut<physics::CharacterMotion>()->crouch = true;
    // Geduckt mit 2 m/s unter die Decke.
    scene.run(character, {1.0f, 0.0f, 0.0f}, 120);
    CHECK(character.get<physics::CharacterMotion>()->crouched);
    CHECK(CharacterScene::position(character).x > 2.5);
    // Unter der Decke bleibt sie geduckt.
    character.get_mut<physics::CharacterMotion>()->crouch = false;
    scene.run(character, {}, 5);
    CHECK(character.get<physics::CharacterMotion>()->crouched);
}

TEST_CASE("Characters on terrain are reproducible", "[physics]") {
    const auto run = [] {
        const auto types = make_types();
        world::World world{types};
        add_flat_terrain(world, {});
        world::Entity character = add_character(world, {0.0, 9.0, 0.0});
        jobs::JobSystem jobs{{.worker_count = 3}};
        world::Simulation simulation{world, jobs};
        physics::PhysicsWorld physics{world, jobs, {.wait_for_terrain = true}};
        physics::add_to(simulation, physics);
        for (i32 i = 0; i < 180; ++i) {
            if (character.has<physics::CharacterMotion>()) {
                auto* motion = character.get_mut<physics::CharacterMotion>();
                motion->move = {0.7f, 0.0f, 0.7f};
                motion->jump = i == 90;
            }
            frame(world, jobs, simulation);
        }
        return character.get<world::Transform>()->position;
    };
    const math::DVec3 first = run();
    const math::DVec3 second = run();
    CHECK(first.y > 7.9);
    CHECK(first.x > 4.0);
    CHECK(first.x == second.x);
    CHECK(first.y == second.y);
    CHECK(first.z == second.z);
}

TEST_CASE("Terrain materials set friction, ray hits and the ground of characters", "[physics]") {
    // Das ebene Terrain liegt unter der Sandgrenze, oben ist also Sand. Eine Kiste rutscht mit
    // Anfangsgeschwindigkeit darüber; rutschiger Sand trägt sie weiter.
    const auto slide = [](f32 friction) {
        const auto types = make_types();
        world::World world{types};
        add_flat_terrain(world, {});
        world::Entity box = world.create();
        box.set(world::Transform{.position = {-4.0, 8.52, 0.0}})
            .set(physics::RigidBody{.friction = 1.0f, .linear_velocity = {6.0f, 0.0f, 0.0f}})
            .set(physics::Collider{.shape = physics::ShapeType::Box});
        jobs::JobSystem jobs{{.worker_count = 2}};
        world::Simulation simulation{world, jobs};
        physics::PhysicsWorld physics{world, jobs, {.wait_for_terrain = true}};
        physics::add_to(simulation, physics);
        world::TerrainMaterials table = world::standard_terrain_materials();
        table.materials[voxel::sand].friction = friction;
        physics.set_terrain_materials(table);
        for (i32 i = 0; i < 120; ++i) {
            frame(world, jobs, simulation);
        }
        return box.get<world::Transform>()->position.x;
    };
    const f64 slippery = slide(0.02f);
    const f64 rough = slide(1.0f);
    CHECK(slippery > rough + 1.0);

    // Strahl und Figur kennen das Material unter sich.
    const auto types = make_types();
    world::World world{types};
    add_flat_terrain(world, {});
    world::Entity character = add_character(world, {0.0, 8.5, 0.0});
    jobs::JobSystem jobs{{.worker_count = 2}};
    world::Simulation simulation{world, jobs};
    physics::PhysicsWorld physics{world, jobs, {.wait_for_terrain = true}};
    physics::add_to(simulation, physics);
    for (i32 i = 0; i < 60; ++i) {
        frame(world, jobs, simulation);
    }
    const auto hit = physics.raycast({3.0, 12.0, 3.0}, {0.0f, -1.0f, 0.0f}, 10.0f);
    REQUIRE(hit);
    CHECK(hit->material == voxel::sand);
    const auto* motion = character.get<physics::CharacterMotion>();
    REQUIRE(motion != nullptr);
    CHECK(motion->ground == physics::GroundState::Ground);
    CHECK(motion->ground_material == voxel::sand);
}
