module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
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
//
// Seit R5.1 rechnet app::GameWorld die Welt wie in Sandbox, Editor und auf dem Server, aus der
// Szene szenen/spielplatz.json mit allen Figuren und Skripten. Die Spur kommt als Aktionen der
// Figur (PlaygroundTrack), so wie die Eingabe eines Spielers auf dem Server; --server --track
// rechnet dieselbe Spur und kommt auf dieselbe Prüfsumme.
export module sandbox.playground;

import ngine.core;
import ngine.asset;
import ngine.filesystem;
import ngine.input;
import ngine.platform;
import ngine.script;
import ngine.jobs;
import ngine.math;
import ngine.physics;
import ngine.profile;
import ngine.reflect;
import ngine.render;
import ngine.world;
import ngine.audio;
import ngine.animation;
import app.game_world;
import app.model_scene;
import app.paths;
import app.physics;
import app.player;
import app.scripting;

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

// Dieselbe Spur als Ereignisse der Eingabe für die Skripte (Spielersteuerung.cs, R4.4): W, Shift,
// Leertaste, Maus und Strg nach der Standardbelegung. frame zu 1/60 s.
export std::vector<platform::Event> scripted_events(u64 frame) {
    std::vector<platform::Event> events;
    const auto edge = [&](u64 from, u64 to, platform::Key key) {
        if (frame == from) {
            events.emplace_back(platform::event::KeyPressed{key});
        } else if (frame == to) {
            events.emplace_back(platform::event::KeyReleased{key});
        }
    };
    edge(0, 540, platform::Key::W);
    edge(120, 240, platform::Key::Shift);
    edge(150, 151, platform::Key::Space);
    edge(400, 401, platform::Key::Space);
    edge(420, 480, platform::Key::Ctrl);
    if (frame >= 240 && frame < 360) {
        events.emplace_back(platform::event::MouseMoved{.x = 0.0f, .y = 0.0f, .dx = 5.5f});
    }
    return events;
}

// Feste Spur der Spielfigur (R4.3, seit R5.1 als Aktionen der Figur): laufen, rennen, springen,
// drehen, ducken; nach sechs Sekunden gräbt sie unter der ersten losen Kiste, die in die Grube
// fällt. Steuert ein Skript die Figur, liest es die Aktionen, sonst GameWorld::steer_players().
// Ein Aufruf von apply() pro festem Schritt, vor GameWorld::update().
export class PlaygroundTrack {
public:
    PlaygroundTrack(app::GameWorld& game, world::Entity figure)
        : game_{&game}, figure_{figure}, actions_{input::ActionMap::standard()} {
        game.bind_player(figure, actions_);
    }
    ~PlaygroundTrack() {
        game_->unbind_player(figure_);
    }
    PlaygroundTrack(const PlaygroundTrack&) = delete;
    PlaygroundTrack& operator=(const PlaygroundTrack&) = delete;

    void apply(u64 frame) {
        actions_.begin_frame();
        for (const platform::Event& event : scripted_events(frame)) {
            actions_.handle(event);
        }
        actions_.end_frame(1.0f / 60.0f);
        if (frame == 360) {
            dig_under_box();
        }
    }

    [[nodiscard]] world::Entity figure() const noexcept {
        return figure_;
    }
    [[nodiscard]] u64 dug() const noexcept {
        return dug_;
    }

private:
    void dig_under_box() {
        world::World& world = game_->world();
        world::Entity terrain;
        world::Entity box;
        world.each<const world::Terrain>([&](world::Entity entity, const world::Terrain&) {
            if (!terrain || entity.id() < terrain.id()) {
                terrain = entity;
            }
        });
        world.each<const world::Name>([&](world::Entity entity, const world::Name& name) {
            if (name.value == "Lose Kiste 1") {
                box = entity;
            }
        });
        if (!terrain || !box) {
            log::warn("Spur: kein Terrain oder keine lose Kiste zum Graben");
            return;
        }
        const math::DVec3 at = box.get<world::Transform>()->position;
        app::PlayerInput input;
        input.dig_radius = 1.4f;
        app::dig({.terrain = terrain, .point = at - math::DVec3{0.0, 0.6, 0.0}}, input, false);
        ++dug_;
    }

    app::GameWorld* game_;
    world::Entity figure_;
    input::ActionState actions_;
    u64 dug_ = 0;
};

export int run_playground_headless(u64 frames, math::DVec3 origin, u32 extra_bodies, u32 characters,
                                   bool use_scripts, bool use_audio,
                                   const asset::AssetDatabase* database,
                                   const asset::AssetCache* cache) {
    // Vor der Welt angelegt, damit sie nach deren Skripten endet.
    const std::unique_ptr<script::ScriptRuntime> runtime =
        use_scripts ? app::start_script_runtime() : nullptr;
    app::GameWorld game{{.database = database, .cache = cache, .deterministic = true}};
    world::World& world = game.world();
    physics::PhysicsWorld& physics = game.physics();
    world::Simulation& simulation = game.simulation();
    animation::AnimationSystem& animation = game.animation();
    // Ohne Renderer laufen nur die Automaten; Posen rechnet das System nur für pose_mode Always.
    // Es schreibt nichts in die Welt, die Prüfsumme bleibt.
    animation.set_rendering(false);
    // Skripte (R4.4): Gibt es .NET und assets/skripte, steuert Spielersteuerung.cs die Figur mit
    // der Spur als Eingabe; sonst die Steuerung in C++. Auf den Bau wird gewartet, damit die Spur
    // ab dem ersten Bild gilt und die Szene die Skript-Komponenten kennt.
    if (runtime) {
        game.enable_scripts(*runtime, nullptr, {.watch = false, .wait = true});
    }
    const app::PlaygroundInfo info = game.load_playground(origin, extra_bodies);
    world::Entity player = app::find_player(world);
    // Audio (R4.5) über das Null-Gerät: Radio, Wind, Summen und Schritte der Figur. Es liest die
    // Welt nur; mit --no-audio bleibt die Prüfsumme gleich.
    std::optional<audio::AudioEngine> audio_engine;
    std::optional<audio::AudioSystem> audio_system;
    if (use_audio) {
        audio_engine.emplace(audio::AudioOptions{.mode = audio::DeviceMode::Null});
        audio::register_loaders(game.resources());
        audio_system.emplace(*audio_engine, game.resources(), database);
        game.bind_audio(&*audio_system, &*audio_engine);
    }
    // Viele Figuren im Kreis um den Startpunkt, jede mit eigener Richtung (R4.3, Messung für R5).
    std::vector<world::Entity> crowd;
    for (u32 i = 0; i < characters; ++i) {
        const f64 angle = static_cast<f64>(i) * 2.399963;
        const f64 radius = 3.0 + 1.5 * std::sqrt(static_cast<f64>(i));
        world::Entity figure = world.create(std::format("Figur {}", i + 1));
        figure
            .set(
                world::Transform{.position = info.spawn + math::DVec3{std::cos(angle) * radius, 3.0,
                                                                      std::sin(angle) * radius}})
            .set(physics::CharacterController{});
        crowd.push_back(figure);
    }
    std::optional<PlaygroundTrack> track;
    if (player) {
        track.emplace(game, player);
    } else {
        log::warn("Spielplatz: keine Spielfigur in der Szene");
    }
    log::info("Spielplatz: {}, Steuerung {}", info.from_scene ? "aus der Szene" : "aus Code",
              player && game.scripted_player(player) ? "aus C# (Spielersteuerung.cs)" : "in C++");
    u64 landed = 0;
    u64 steps = 0;
    events::EventBus& bus = game.events();
    const auto on_landed =
        bus.subscribe<physics::CharacterLanded>([&](const physics::CharacterLanded&) { ++landed; });
    const auto on_step =
        bus.subscribe<physics::CharacterStep>([&](const physics::CharacterStep&) { ++steps; });
    u64 animation_events = 0;
    const auto on_animation_event = bus.subscribe<animation::AnimationEvent>(
        [&](const animation::AnimationEvent&) { ++animation_events; });

    // Ein Bild ist 1/60 s, also ein fester Schritt pro Bild, so schnell es geht.
    Timing step;
    Timing sync;
    Timing write_back;
    Timing transforms;
    Timing figures;
    usize one_shots = 0;
    const auto start = std::chrono::steady_clock::now();
    for (u64 frame = 0; frame < frames; ++frame) {
        NGINE_PROFILE_ZONE("Frame");
        if (track) {
            track->apply(frame);
        }
        for (usize i = 0; i < crowd.size(); ++i) {
            if (auto* motion = crowd[i].get_mut<physics::CharacterMotion>()) {
                const f64 angle = static_cast<f64>(frame) / 60.0 * 0.8 + static_cast<f64>(i);
                motion->move = {static_cast<f32>(std::cos(angle)), 0.0f,
                                static_cast<f32>(std::sin(angle))};
            }
        }
        game.steer_players();
        (void)game.update(1.0 / 60.0);
        const physics::StepStats& stats = physics.stats();
        step.add(stats.sync_ms + stats.update_ms + stats.write_back_ms + stats.characters_ms);
        sync.add(stats.sync_ms);
        write_back.add(stats.write_back_ms);
        figures.add(stats.characters_ms);
        transforms.add(simulation.stats().transforms_ms);
        if (audio_system) {
            if (player) {
                audio_system->set_fallback_listener(player.get<world::WorldTransform>()->value);
            }
            audio_system->update(world, 1.0 / 60.0);
            audio_engine->update();
            one_shots = std::max(one_shots, audio_system->stats().one_shots);
        }
        profile::frame_mark();
    }
    const std::chrono::duration<f64, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    const physics::StepStats& stats = physics.stats();
    log::info(
        "Spielplatz: {} feste Schritte ({:.2f} s simuliert) in {:.0f} ms mit {} Worker-Threads",
        simulation.time().tick, simulation.time().elapsed, elapsed.count(),
        game.jobs().worker_count());
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
              landed, steps, track ? track->dug() : 0);
    if (player) {
        const math::DVec3 at = player.get<world::Transform>()->position - info.spawn;
        const auto* motion = player.get<physics::CharacterMotion>();
        log::info("Spielfigur: {:.2f} m vom Start ({:.2f}, {:.2f}, {:.2f}), {}", math::length(at),
                  at.x, at.y, at.z,
                  motion != nullptr && motion->ground == physics::GroundState::Ground
                      ? "auf dem Boden"
                      : "nicht auf dem Boden");
    }
    if (audio_system) {
        const audio::AudioStats audio_stats = audio_engine->stats();
        log::info("Audio: {} Quellen spielen, bis zu {} Einmal-Klänge gleichzeitig, {} Stimmen im "
                  "Mixer, {} Leerläufe",
                  audio_system->stats().playing, one_shots, audio_stats.voices,
                  audio_stats.mixer.underruns);
        audio_system->stop_all();
        game.bind_audio(nullptr, nullptr);
    }
    const animation::AnimationStats& animation_stats = animation.stats();
    log::info("Animation: {} Animatoren, {} mit Automat, {} mit Pose, {} Ereignisse, zuletzt "
              "{:.3f} ms pro Bild",
              animation_stats.animators, animation_stats.running, animation_stats.posed,
              animation_events, animation_stats.update_ms);
    log::info("Prüfsumme der Lagen: {:016x}", physics::checksum(world));
    return 0;
}

// Spielplatz aus Code als Szene (R4.3): mit der Figur aus prefabs/spielfigur.prefab, wenn es den
// Asset-Ordner gibt. So entsteht assets/szenen/spielplatz.json. Seit R4.5 mit Radio, Wind und
// kreisendem Summen; die Typen stehen in derselben Reihenfolge wie in der Szene der App.
// Felder von Spaziergang.cs für save_playground(), das ohne C# läuft: Mit diesem Stellvertreter
// stehen die Werte jeder Figur als Abweichung vom Prefab in der Szene. Namen und Typen wie im
// Skript.
struct WalkFields {
    f32 radius = 2.0f;
    f32 tempo = 1.6f;
    f32 richtung = 1.0f;
    f32 glaettung = 10.0f;
};

export int save_playground(const std::string& path, const asset::AssetDatabase* database) {
    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    physics::register_types(types);
    app::register_types(types);
    app::register_player_types(types);
    audio::register_types(types);
    animation::register_types(types);
    // Wie die Skripte in der App nach den Typen der Engine.
    types.add<WalkFields>("Spaziergang")
        .field("Radius", &WalkFields::radius)
        .field("Tempo", &WalkFields::tempo)
        .field("Richtung", &WalkFields::richtung)
        .field("Glaettung", &WalkFields::glaettung);
    world::World world{types};
    jobs::JobSystem jobs;
    const app::PlaygroundLayout layout = app::build_playground(world, {0.0, 0.0, 0.0}, jobs);
    app::add_playground_audio(world, layout, database);
    app::add_playground_hum(world, layout, database);
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
    // Laterne neben dem Start (R4.4), schaltbar mit light (Laterne.cs).
    const asset::AssetInfo* lantern_info =
        database != nullptr ? database->find_path("prefabs/laterne.prefab") : nullptr;
    std::optional<world::Prefab> lantern;
    if (lantern_info != nullptr) {
        if (auto loaded = world::load_prefab_file(lantern_info->file)) {
            lantern = *std::move(loaded);
            if (auto placed = world::instantiate(world, *lantern, lantern_info->id)) {
                world::Transform transform = *placed->get<world::Transform>();
                transform.position = layout.lantern;
                placed->set(transform);
            } else {
                log::warn("Laterne: {}", placed.error());
            }
        } else {
            log::warn("Laterne: {}", loaded.error());
        }
    }
    // Drei Figuren auf der Ebene (R4.6, prefabs/spaziergaenger.prefab mit Spaziergang.cs): eine
    // geht, eine rennt im Kreis, eine steht. Position ist der Startpunkt auf dem Kreis.
    const asset::AssetInfo* walker_info =
        database != nullptr ? database->find_path("prefabs/spaziergaenger.prefab") : nullptr;
    std::optional<world::Prefab> walker;
    if (walker_info != nullptr) {
        if (auto loaded = world::load_prefab_file(walker_info->file)) {
            walker = *std::move(loaded);
            struct Walk {
                math::DVec3 start;
                f32 tempo;
                f32 radius;
            };
            const Walk walks[] = {{{7.8, 0.0, 2.3}, 1.6f, 1.8f},
                                  {{2.7, 0.0, 2.3}, 5.5f, 2.2f},
                                  {{-6.0, 0.0, 1.5}, 0.0f, 0.0f}};
            for (const Walk& walk : walks) {
                auto placed = world::instantiate(world, *walker, walker_info->id);
                if (!placed) {
                    log::warn("Spaziergänger: {}", placed.error());
                    break;
                }
                world::Transform transform = *placed->get<world::Transform>();
                transform.position = layout.platform + walk.start;
                placed->set(transform);
                auto* fields = placed->get_mut<WalkFields>();
                if (fields != nullptr) {
                    fields->tempo = walk.tempo;
                    fields->radius = walk.radius;
                    placed->modified<WalkFields>();
                }
            }
        } else {
            log::warn("Spaziergänger: {}", loaded.error());
        }
    }
    const world::PrefabLookup lookup = [&](u64 id) -> const world::Prefab* {
        if (prefab && id == prefab_id) {
            return &*prefab;
        }
        if (walker && walker_info != nullptr && id == walker_info->id) {
            return &*walker;
        }
        return lantern && lantern_info != nullptr && id == lantern_info->id ? &*lantern : nullptr;
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
