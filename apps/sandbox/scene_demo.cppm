module;

#include <ngine/profile.hpp>

#include <chrono>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>

// Demo für ECS, Reflection und Szenen: Ein kleines Sonnensystem mit Planeten, Monden und einem
// Asteroidengürtel bewegt sich einige Frames lang über Systeme auf dem Job-System. Danach wird die
// Szene gespeichert, in eine neue Welt geladen und mit dem Original verglichen.
export module sandbox.scene_demo;

import ngine.core;
import ngine.reflect;
import ngine.jobs;
import ngine.filesystem;
import ngine.profile;
import ngine.world;
import sandbox.frame_stats;

namespace sandbox {

namespace {

using namespace ngine;

// Bahn um das Elternteil. Der Radius ist f64, weil Planetenbahnen in Metern f32 sprengen.
struct Orbit {
    f64 radius = 0.0;
    f32 speed = 0.0f;
    f32 angle = 0.0f;
};

// Drehung um die eigene Achse.
struct Spin {
    f32 speed = 0.0f;
    f32 angle = 0.0f;
};

struct Body {
    f32 mass = 0.0f;
    bool has_atmosphere = false;
};

constexpr u32 asteroid_count = 2000;
constexpr f32 tau = 6.28318530718f;

void register_demo_types(reflect::TypeRegistry& types) {
    world::register_types(types);
    types.add<Orbit>("Orbit")
        .field("radius", &Orbit::radius)
        .field("speed", &Orbit::speed)
        .field("angle", &Orbit::angle);
    types.add<Spin>("Spin").field("speed", &Spin::speed).field("angle", &Spin::angle);
    types.add<Body>("Body")
        .field("mass", &Body::mass)
        .field("has_atmosphere", &Body::has_atmosphere);
}

void build_scene(world::World& world) {
    world::Entity sun = world.create("Sonne");
    sun.set(Body{1.989e30f, false}).set(Spin{0.01f, 0.0f});

    struct PlanetDesc {
        const char* name;
        f64 radius;
        f32 mass;
        bool atmosphere;
        u32 moons;
    };
    constexpr PlanetDesc planets[] = {
        {"Merkur", 5.79e10, 3.30e23f, false, 0},  {"Venus", 1.082e11, 4.87e24f, true, 0},
        {"Erde", 1.496e11, 5.97e24f, true, 1},    {"Mars", 2.279e11, 6.42e23f, true, 2},
        {"Jupiter", 7.785e11, 1.90e27f, true, 4}, {"Saturn", 1.434e12, 5.68e26f, true, 3},
    };
    for (u32 p = 0; p < std::size(planets); ++p) {
        const PlanetDesc& desc = planets[p];
        world::Entity planet = world.create(desc.name);
        planet.set(Orbit{desc.radius, 1.0f / static_cast<f32>(p + 1), static_cast<f32>(p)})
            .set(Spin{0.5f, 0.0f})
            .set(Body{desc.mass, desc.atmosphere})
            .set_parent(sun);
        for (u32 m = 0; m < desc.moons; ++m) {
            world.create(std::string{desc.name} + " Mond " + std::to_string(m + 1))
                .set(Orbit{4e8 * (m + 1), 2.0f, static_cast<f32>(m)})
                .set_parent(planet);
        }
    }

    world::Entity belt = world.create("Asteroidengürtel");
    belt.set_parent(sun);
    for (u32 i = 0; i < asteroid_count; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(asteroid_count);
        world.create()
            .set(Orbit{3.3e11 + 1e11 * std::sin(t * 37.0f), 0.2f + 0.1f * t, t * tau})
            .set(Spin{1.0f + t, 0.0f})
            .set_parent(belt);
    }
}

} // namespace

// Führt die Demo aus. path: Zieldatei, sonst im Benutzerordner. Liefert den Exit-Code.
export int run_scene_demo(u64 frames, std::optional<std::string> path) {
    profile::set_thread_name("Hauptthread");

    reflect::TypeRegistry types;
    register_demo_types(types);

    world::World world{types};
    build_scene(world);
    log::info("Szenen-Demo: {} Entities, {} Frames", world.entity_count(), frames);

    jobs::JobSystem job_system;
    world::Schedule schedule{world};
    f32 delta = 1.0f / 60.0f;
    schedule.add<Orbit>(
        "Umlaufbahnen",
        [&delta](Orbit& orbit) { orbit.angle = std::fmod(orbit.angle + orbit.speed * delta, tau); },
        {.grain = 256});
    // Unabhängig von den Umlaufbahnen, läuft also gleichzeitig.
    schedule.add<Spin>(
        "Eigendrehung",
        [&delta](Spin& spin) { spin.angle = std::fmod(spin.angle + spin.speed * delta, tau); },
        {.grain = 256});

    const auto start = std::chrono::steady_clock::now();
    for (u64 frame = 0; frame < frames; ++frame) {
        NGINE_PROFILE_ZONE("Frame");
        schedule.run(job_system);
        report_memory();
        profile::frame_mark();
    }
    const std::chrono::duration<f64, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    log::info("Systeme: {:.3f} ms pro Frame mit {} Worker-Threads",
              frames > 0 ? elapsed.count() / static_cast<f64>(frames) : 0.0,
              job_system.worker_count());

    if (!path) {
        auto dir = filesystem::user_data_dir("NGine");
        if (!dir) {
            log::error("Kein Benutzerordner: {}", dir.error());
            return 1;
        }
        path = filesystem::join(*dir, "sandbox.scene.json");
    }

    if (auto saved = world::save_scene_file(world, *path); !saved) {
        log::error("Speichern fehlgeschlagen: {}", saved.error());
        return 1;
    }
    log::info("Szene gespeichert: {}", *path);

    world::World loaded{types};
    const auto count = world::load_scene_file(loaded, *path);
    if (!count) {
        log::error("Laden fehlgeschlagen: {}", count.error());
        return 1;
    }
    if (world::save_scene(loaded) != world::save_scene(world)) {
        log::error("Geladene Szene weicht vom Original ab");
        return 1;
    }
    log::info("Szene geladen: {} Entities, gleich dem Original", *count);
    return 0;
}

} // namespace sandbox
