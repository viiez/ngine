module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

// Spielplatz der Physik ohne Fenster und GPU (--playground --headless, R4.2): dieselbe Welt wie in
// der Sandbox mit Fenster, gerechnet über die Spielschleife. Meldet am Ende eine Prüfsumme der
// Lagen (gleiche Szene, gleicher Build: gleiche Summe) und mit --bodies die Zeiten von Physik und
// update_transforms(). Seit R4.3 auf dem Terrain: Die Spielfigur läuft eine feste Spur (laufen,
// rennen, springen, drehen, ducken) und gräbt unter einer losen Kiste; die Physik wartet auf die
// Formen der Chunks, damit die Summe nicht von der Zahl der Threads abhängt. --characters lässt
// viele Figuren im Kreis laufen und misst sie; --save-playground schreibt die Szene.
export module sandbox.playground;

import ngine.core;
import ngine.asset;
import ngine.jobs;
import ngine.math;
import ngine.physics;
import ngine.profile;
import ngine.reflect;
import ngine.render;
import ngine.world;
import app.physics;
import app.player;

namespace sandbox {

using namespace ngine;

namespace {

struct Timing {
    std::vector<f64> values;

    void add(f64 value) {
        values.push_back(value);
    }
    [[nodiscard]] f64 percentile(f64 p) {
        if (values.empty()) {
            return 0.0;
        }
        std::ranges::sort(values);
        const auto index = static_cast<usize>(p * static_cast<f64>(values.size() - 1) + 0.5);
        return values[std::min(index, values.size() - 1)];
    }
};

} // namespace

// Steuerung der Figur auf der festen Spur, frame zu 1/60 s.
app::PlayerControls scripted_controls(u64 frame) {
    const f64 t = static_cast<f64>(frame) / 60.0;
    app::PlayerControls controls;
    if (t < 9.0) {
        controls.forward = 1.0f;
    }
    controls.run = t >= 2.0 && t < 4.0;
    controls.jump = frame == 150 || frame == 400;
    if (t >= 4.0 && t < 6.0) {
        // Gut 90 Grad nach rechts über zwei Sekunden.
        controls.look_x = 5.5f;
    }
    controls.crouch = t >= 7.0 && t < 8.0;
    return controls;
}

export int run_playground_headless(u64 frames, math::DVec3 origin, u32 extra_bodies,
                                   u32 characters) {
    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    physics::register_types(types);
    app::register_player_types(types);
    world::World world{types};
    jobs::JobSystem jobs;
    const app::PlaygroundLayout layout = app::build_playground(world, origin, jobs);
    world::Entity player = app::add_player(world, layout.spawn);
    if (extra_bodies > 0) {
        app::rain_bodies(world, layout.spawn + math::DVec3{0.0, 2.0, 14.0}, extra_bodies);
    }
    // Viele Figuren im Kreis um den Startpunkt, jede mit eigener Richtung (R4.3, Messung für R5).
    std::vector<world::Entity> crowd;
    for (u32 i = 0; i < characters; ++i) {
        const f64 angle = static_cast<f64>(i) * 2.399963;
        const f64 radius = 3.0 + 1.5 * std::sqrt(static_cast<f64>(i));
        world::Entity figure = world.create(std::format("Figur {}", i + 1));
        figure
            .set(world::Transform{.position =
                                      layout.spawn + math::DVec3{std::cos(angle) * radius, 3.0,
                                                                 std::sin(angle) * radius}})
            .set(physics::CharacterController{});
        crowd.push_back(figure);
    }
    world::Simulation simulation{world, jobs};
    app::ModelShapes shapes;
    shapes.add(app::playground_id, app::playground_model());
    physics::PhysicsWorld physics{world, jobs, {.wait_for_terrain = true}};
    physics.set_shape_source(shapes.source());
    physics::add_to(simulation, physics);
    u64 landed = 0;
    u64 steps = 0;
    u64 dug = 0;
    events::EventBus bus;
    physics.set_events(&bus);
    const auto on_landed =
        bus.subscribe<physics::CharacterLanded>([&](const physics::CharacterLanded&) { ++landed; });
    const auto on_step =
        bus.subscribe<physics::CharacterStep>([&](const physics::CharacterStep&) { ++steps; });

    // Ein Bild ist 1/60 s, also ein fester Schritt pro Bild, so schnell es geht.
    Timing step;
    Timing sync;
    Timing write_back;
    Timing transforms;
    Timing figures;
    const auto start = std::chrono::steady_clock::now();
    for (u64 frame = 0; frame < frames; ++frame) {
        NGINE_PROFILE_ZONE("Frame");
        app::apply_controls(player, scripted_controls(frame));
        // Nach sechs Sekunden: unter der ersten losen Kiste graben, sie fällt in die Grube.
        if (frame == 360 && !layout.loose_boxes.empty()) {
            const math::DVec3 box = layout.loose_boxes.front().get<world::Transform>()->position;
            app::PlayerInput input;
            input.dig_radius = 1.4f;
            app::dig({.terrain = layout.terrain, .point = box - math::DVec3{0.0, 0.6, 0.0}}, input,
                     false);
            ++dug;
        }
        for (usize i = 0; i < crowd.size(); ++i) {
            if (auto* motion = crowd[i].get_mut<physics::CharacterMotion>()) {
                const f64 angle = static_cast<f64>(frame) / 60.0 * 0.8 + static_cast<f64>(i);
                motion->move = {static_cast<f32>(std::cos(angle)), 0.0f,
                                static_cast<f32>(std::sin(angle))};
            }
        }
        (void)world::update_transforms(world);
        (void)world::update_terrains(world, jobs, {.wait = true});
        (void)simulation.advance(1.0 / 60.0);
        const physics::StepStats& stats = physics.stats();
        step.add(stats.sync_ms + stats.update_ms + stats.write_back_ms + stats.characters_ms);
        sync.add(stats.sync_ms);
        write_back.add(stats.write_back_ms);
        figures.add(stats.characters_ms);
        transforms.add(simulation.stats().transforms_ms);
        profile::frame_mark();
    }
    const std::chrono::duration<f64, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    const physics::StepStats& stats = physics.stats();
    log::info(
        "Spielplatz: {} feste Schritte ({:.2f} s simuliert) in {:.0f} ms mit {} Worker-Threads",
        simulation.time().tick, simulation.time().elapsed, elapsed.count(), jobs.worker_count());
    log::info("Physik: {} Körper, am Ende {} wach; Schritt Median {:.2f} ms, 99. Perzentil {:.2f} "
              "ms (Abgleich Median {:.2f} ms, Rückschrieb Median {:.2f} ms); update_transforms "
              "Median {:.2f} ms, 99. Perzentil {:.2f} ms",
              physics.body_count(), stats.active_bodies, step.percentile(0.5),
              step.percentile(0.99), sync.percentile(0.5), write_back.percentile(0.5),
              transforms.percentile(0.5), transforms.percentile(0.99));
    log::info("Terrain-Kollision: {} Formen ({:.1f} MiB), {} eingefroren", stats.terrain_chunks,
              static_cast<f64>(stats.terrain_bytes) / (1024.0 * 1024.0), stats.frozen_bodies);
    const f64 per_figure =
        stats.characters > 0 ? figures.percentile(0.5) / static_cast<f64>(stats.characters) : 0.0;
    log::info("Figuren: {}; Zeit Median {:.2f} ms, 99. Perzentil {:.2f} ms, pro Figur {:.3f} ms; "
              "{} Landungen, {} Schritte, {} Mal gegraben",
              stats.characters, figures.percentile(0.5), figures.percentile(0.99), per_figure,
              landed, steps, dug);
    const math::DVec3 at = player.get<world::Transform>()->position - layout.spawn;
    const auto* motion = player.get<physics::CharacterMotion>();
    log::info("Spielfigur: {:.2f} m vom Start ({:.2f}, {:.2f}, {:.2f}), {}", math::length(at), at.x,
              at.y, at.z,
              motion != nullptr && motion->ground == physics::GroundState::Ground
                  ? "auf dem Boden"
                  : "nicht auf dem Boden");
    log::info("Prüfsumme der Lagen: {:016x}", physics::checksum(world));
    return 0;
}

// Spielplatz aus Code als Szene (R4.3): mit der Figur aus prefabs/spielfigur.prefab, wenn es den
// Asset-Ordner gibt. So entsteht assets/szenen/spielplatz.json.
export int save_playground(const std::string& path, const asset::AssetDatabase* database) {
    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    physics::register_types(types);
    app::register_player_types(types);
    world::World world{types};
    jobs::JobSystem jobs;
    const app::PlaygroundLayout layout = app::build_playground(world, {0.0, 0.0, 0.0}, jobs);
    const asset::AssetInfo* info =
        database != nullptr ? database->find_path("prefabs/spielfigur.prefab") : nullptr;
    std::optional<world::Prefab> prefab;
    if (info != nullptr) {
        if (auto loaded = world::load_prefab_file(info->file)) {
            prefab = *std::move(loaded);
        } else {
            log::warn("Spielfigur: {}", loaded.error());
        }
    }
    const u64 prefab_id = info != nullptr ? info->id : 0;
    (void)app::spawn_player(world, layout.spawn, prefab ? &*prefab : nullptr, prefab_id);
    const world::PrefabLookup lookup = [&](u64 id) -> const world::Prefab* {
        return prefab && id == prefab_id ? &*prefab : nullptr;
    };
    if (auto saved = world::save_scene_file(world, path, lookup); !saved) {
        log::error("Spielplatz: {}", saved.error());
        return 1;
    }
    log::info("Spielplatz gespeichert: {} (Figur {})", path,
              prefab ? "aus dem Prefab" : "aus Code");
    return 0;
}

} // namespace sandbox
