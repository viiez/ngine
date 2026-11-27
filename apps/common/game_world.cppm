module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Simulation der Spielwelt ohne Fenster und GPU (R5.1, TD-64): Welt, Spielschleife, Physik mit
// Terrain-Kollision, Skripte, Automaten der Animation, Prefabs und die Materialtabelle des
// Terrains. Sandbox und Editor legen in ModelScene Renderer, Audio und Kamera darüber,
// `--headless --playground` und der dedizierte Server nutzen sie direkt. So rechnen alle dieselbe
// Welt auf demselben Weg, und die Stufen des Netzwerks hängen sich an einer Stelle ein.
//
// Bis R4.6 bauten ModelScene und sandbox/playground.cppm das je für sich zusammen.
export module app.game_world;

import ngine.core;
import ngine.animation;
import ngine.asset;
import ngine.audio;
import ngine.filesystem;
import ngine.input;
import ngine.jobs;
import ngine.math;
import ngine.physics;
import ngine.profile;
import ngine.reflect;
import ngine.render;
import ngine.script;
import ngine.world;
import app.prefabs;
import app.physics;
import app.player;
import app.terrain_materials;

namespace app {

using namespace ngine;
using math::DVec3;
using math::Vec3;

// Kreisbahn um center mit Auf und Ab, z. B. für die Lichter der Standardszene. Das System
// „Kreisbahnen“ zählt time weiter und setzt daraus Transform; time gehört zum Zustand, damit
// Spielen im Editor und Stoppen ihn mit der Welt sichern und wiederherstellen.
export struct Orbit {
    // Mittelpunkt der Bahn auf der untersten Höhe.
    DVec3 center{0.0, 0.0, 0.0};
    f32 radius_x = 1.0f;
    f32 radius_z = 1.0f;
    // Hub des Auf und Ab in Metern.
    f32 bob = 0.0f;
    // Winkelgeschwindigkeit der Bahn und des Auf und Ab im Bogenmaß pro Sekunde.
    f32 speed = 0.4f;
    f32 bob_speed = 0.9f;
    // Startwinkel.
    f32 phase = 0.0f;
    // Zeit auf der Bahn in Sekunden.
    f32 time = 0.0f;
};

// Ort auf der Bahn zur Zeit orbit.time.
export DVec3 orbit_position(const Orbit& orbit) {
    const f32 angle = orbit.phase + orbit.time * orbit.speed;
    const f32 height =
        orbit.bob * (0.5f + 0.5f * std::sin(orbit.time * orbit.bob_speed + orbit.phase * 2.0f));
    return orbit.center + DVec3{std::cos(angle) * static_cast<f64>(orbit.radius_x), height,
                                std::sin(angle) * static_cast<f64>(orbit.radius_z)};
}

// Registriert die Komponenten der Anwendungen (Orbit) in der Reflection.
export void register_types(reflect::TypeRegistry& types) {
    if (types.find<Orbit>() == nullptr) {
        types.add<Orbit>("Orbit")
            .field("center", &Orbit::center)
            .field("radius_x", &Orbit::radius_x)
            .range(0.0, 100.0)
            .field("radius_z", &Orbit::radius_z)
            .range(0.0, 100.0)
            .field("bob", &Orbit::bob)
            .range(0.0, 100.0)
            .field("speed", &Orbit::speed)
            .field("bob_speed", &Orbit::bob_speed)
            .field("phase", &Orbit::phase)
            .hint(reflect::Hint::Angle)
            .field("time", &Orbit::time);
    }
}

// Alle Typen, die Szenen der Anwendungen enthalten können, in der Reihenfolge, in der Sandbox,
// Editor und Server sie anmelden (Skripte kommen danach).
export reflect::TypeRegistry make_game_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    render::register_types(types);
    physics::register_types(types);
    register_types(types);
    register_player_types(types);
    audio::register_types(types);
    animation::register_types(types);
    return types;
}

// Summen, das um die Ebene des Spielplatzes kreist (R4.5): zeigt Doppler und Panning. Gut 12 m/s
// auf der Bahn verschieben die Tonhöhe um etwa einen halben Halbton. Ohne Datenbank oder Klang
// entsteht nichts.
export void add_playground_hum(world::World& world, const PlaygroundLayout& layout,
                               const asset::AssetDatabase* database) {
    const asset::AssetInfo* hum =
        database != nullptr ? database->find_path("klaenge/welt/summen.wav") : nullptr;
    if (hum == nullptr) {
        return;
    }
    const Orbit orbit{.center = layout.platform + DVec3{0.0, 2.5, 0.0},
                      .radius_x = 9.0f,
                      .radius_z = 9.0f,
                      .bob = 1.0f,
                      .speed = 1.4f,
                      .bob_speed = 0.7f};
    world.create("Summer")
        .set(world::Transform{.position = orbit_position(orbit), .scale = {0.3f, 0.3f, 0.3f}})
        .set(render::MeshRenderer{.mesh = playground_ref(playground_ball)})
        .set(orbit)
        .set(audio::AudioSource{.sound = hum->id,
                                .volume = 0.5f,
                                .loop = true,
                                .min_distance = 2.0f,
                                .max_distance = 35.0f});
}

// Wie die Welt ihre Skripte bekommt (R4.4).
export struct ScriptOptions {
    // Mit dem .NET-SDK bauen; sonst nur die Assembly aus dem Cache.
    bool build = true;
    // Bei geänderten .cs-Dateien neu bauen und laden (Hot Reload).
    bool watch = true;
    // Auf den ersten Bau warten (ohne Fenster, damit die Skripte ab dem ersten Bild laufen).
    bool wait = false;
    // Skripte laufen sofort (Spiel, Server); sonst erst nach scripts()->set_active() (Editor).
    bool active = true;
    // Welche Klassen laufen (R5.5): ohne Netz alle, auf dem Server die ohne [Client], auf dem
    // Client die mit [Client] oder [Everywhere].
    script::ScriptRole role = script::ScriptRole::Local;
};

export struct GameWorldDesc {
    // Asset-Ordner; darf fehlen und muss sonst die Welt überleben.
    const asset::AssetDatabase* database = nullptr;
    // Cache gekochter Modelle (Formen der Physik, Rigs); darf fehlen und muss sonst die Welt
    // überleben.
    const asset::AssetCache* cache = nullptr;
    // Worker des Job-Systems; ohne Angabe so viele, wie der Rechner Kerne hat.
    std::optional<u32> worker_count;
    // Fremdes Job-System, das die Welt überleben muss (R5.5: Server- und Client-Welt eines
    // Listen-Servers teilen sich die Worker); worker_count gilt dann nicht.
    jobs::JobSystem* jobs = nullptr;
    // Die Physik wartet auf die Formen der Chunks und das Terrain auf seine Netze, damit das
    // Ergebnis nicht von der Zahl der Threads abhängt (ohne Fenster und auf dem Server). Nach
    // Pinseln wartet keiner von beiden: Bis die neuen Formen und groben Stufen fertig sind, gelten
    // die alten (R5.4, sonst hält jeder Pinsel den Server an).
    bool deterministic = false;
};

// Wo der Spielplatz steht, nachdem load_playground() ihn angelegt hat.
export struct PlaygroundInfo {
    // Fußpunkt der Figur aus der Szene (oder aus Code).
    DVec3 spawn{0.0, 0.0, 0.0};
    // Aus szenen/spielplatz.json statt aus Code.
    bool from_scene = false;
};

export class GameWorld {
public:
    explicit GameWorld(GameWorldDesc desc = {})
        : types_{make_game_types()}, world_{types_},
          own_jobs_{desc.jobs == nullptr ? std::make_unique<jobs::JobSystem>(jobs::JobSystemDesc{
                                               .worker_count = desc.worker_count})
                                         : nullptr},
          jobs_{desc.jobs != nullptr ? desc.jobs : own_jobs_.get()},
          resources_{std::make_unique<asset::ResourceManager>(desc.database, jobs_)},
          prefabs_{*resources_}, simulation_{world_, *jobs_}, shapes_{desc.database, desc.cache},
          physics_{world_, *jobs_}, database_{desc.database}, terrain_materials_{desc.database},
          deterministic_{desc.deterministic} {
        animation::register_loaders(*resources_, desc.cache);
        animation::register_graph_loader(*resources_);
        animation_ = std::make_unique<animation::AnimationSystem>(*resources_, jobs_);
        animation_->set_events(&events_);
        physics_.set_terrain_materials(terrain_materials_.table());
        physics_.set_wait_for_terrain(deterministic_);
        physics_.set_wait_for_rebuilds(false);
        // Feste Simulation mit Interpolation: Kreisbahnen laufen bei jeder Bildrate gleich schnell
        // und ruckeln trotzdem nicht (R4.1).
        simulation_.add<Orbit, world::Transform>("Kreisbahnen",
                                                 [this](Orbit& orbit, world::Transform& transform) {
                                                     orbit.time += world_.time().delta;
                                                     transform.position = orbit_position(orbit);
                                                 },
                                                 {.phase = world::Phase::Fixed});
        physics_.set_shape_source(shapes_.source());
        // Skripte (R4.4): FixedUpdate vor der Physik, Update nach den Systemen eines Bildes.
        // Kontakte und Auslöser erreichen sie über events_.
        simulation_.add_fixed_stage([this] {
            if (scripts_) {
                scripts_->run(script::ScriptPhase::Fixed, world_.time().fixed_delta);
            }
            if (before_physics_) {
                before_physics_();
            }
        });
        simulation_.add_frame_stage([this] {
            if (scripts_) {
                scripts_->run(script::ScriptPhase::Frame, world_.time().delta);
            }
            dig_players(world_.time().delta);
        });
        physics_.set_events(&events_);
        physics::add_to(simulation_, physics_);
        // Flanken der Eingaben gelten bis nach dem festen Schritt, der sie gesehen hat.
        simulation_.set_after_fixed_step([this] {
            if (actions_ != nullptr) {
                actions_->end_fixed_step();
            }
            for (const PlayerSlot& slot : players_) {
                slot.actions->end_fixed_step();
            }
            if (after_step_) {
                after_step_();
            }
        });
        // Modell des Spielplatzes (Kisten, Kugeln, Figur) für die Formen der Physik.
        shapes_.add(playground_id, playground_model());
    }

    ~GameWorld() {
        if (scripts_) {
            scripts_->bind_audio(nullptr, nullptr);
            scripts_->bind_animation(nullptr);
            scripts_->bind(nullptr);
        }
    }

    GameWorld(const GameWorld&) = delete;
    GameWorld& operator=(const GameWorld&) = delete;

    // Skripte aus assets/skripte (R4.4): bauen, laden, bei Änderungen neu laden. runtime muss die
    // Welt überleben. Sie sehen die Welt, actions (alle Skripte, für die keine eigenen Aktionen
    // gebunden sind, siehe bind_player()), die Physik und die Animation. Ohne Asset-Ordner oder
    // Projekt bleibt es bei einer Warnung.
    void enable_scripts(script::ScriptRuntime& runtime, input::ActionState* actions,
                        ScriptOptions options = {}) {
        if (database_ == nullptr) {
            log::warn("Skripte: kein Asset-Ordner");
            return;
        }
        const std::string project = filesystem::join(database_->root(), "skripte/Skripte.csproj");
        if (!filesystem::exists(project)) {
            log::warn("Skripte: {} fehlt", project);
            return;
        }
        scripts_ = std::make_unique<script::ScriptSystem>(
            runtime, types_,
            script::ScriptSystemDesc{.project = project,
                                     .cache_dir =
                                         filesystem::join(database_->root(), ".cache/skripte"),
                                     .build = options.build,
                                     .watch = options.watch,
                                     .role = options.role});
        actions_ = actions;
        scripts_->bind(&world_, actions, &physics_, &events_);
        scripts_->bind_animation(animation_.get());
        for (const PlayerSlot& slot : players_) {
            scripts_->bind_input(slot.figure, slot.actions);
        }
        scripts_->set_active(options.active);
        scripts_->start(options.wait);
    }
    [[nodiscard]] script::ScriptSystem* scripts() noexcept {
        return scripts_.get();
    }
    // Ob Skripte geladen sind und laufen.
    [[nodiscard]] bool scripts_running() const noexcept {
        return scripts_ && scripts_->active() && scripts_->runtime().scripts_loaded();
    }
    // Allgemeine Aktionen der Skripte umbinden (z. B. im Flugmodus keine); die Flanken der
    // Aktionen aus enable_scripts() gibt die Welt weiter frei.
    void bind_script_actions(const input::ActionState* actions) {
        if (scripts_) {
            scripts_->runtime().bind_input(actions);
        }
    }
    // Audio und Materialtabelle für die Skripte (R4.5); nullptr löst es.
    void bind_audio(audio::AudioSystem* audio, audio::AudioEngine* engine) {
        if (scripts_) {
            scripts_->bind_audio(audio, engine,
                                 audio != nullptr ? &terrain_materials_.table() : nullptr);
        }
    }

    // Ob ein laufendes Skript Spielersteuerung die Figur steuert.
    [[nodiscard]] bool scripted_player(world::Entity player) {
        if (!scripts_running()) {
            return false;
        }
        const reflect::TypeInfo* type = types_.find("Spielersteuerung");
        return type != nullptr && !type->retired &&
               world_.has(player.id(), world_.component(*type));
    }

    // Eigene Aktionen für eine Figur (R5.1, Spieler auf dem Server): Ihre Skripte lesen actions,
    // ohne Skripte steuert steer_players() sie damit. actions muss gebunden bleiben, bis
    // unbind_player() sie löst; Flanken gibt die Welt nach jedem festen Schritt frei.
    void bind_player(world::Entity figure, input::ActionState& actions) {
        unbind_player(figure);
        players_.push_back({.figure = figure.id(), .actions = &actions});
        if (scripts_) {
            scripts_->bind_input(figure.id(), &actions);
        }
    }
    void unbind_player(world::Entity figure) {
        std::erase_if(players_, [&](const PlayerSlot& slot) { return slot.figure == figure.id(); });
        if (scripts_) {
            scripts_->bind_input(figure.id(), nullptr);
        }
    }
    // Figuren mit eigenen Aktionen, die kein Skript steuert: Steuerung in C++ wie bis R4.3.
    void steer_players() {
        for (const PlayerSlot& slot : players_) {
            world::Entity figure = world_.entity(slot.figure);
            if (figure && !scripted_player(figure)) {
                apply_controls(figure, player_controls(*slot.actions));
            }
        }
    }

    // Vorhersage auf dem Client (R5.3): before läuft in jedem festen Schritt nach den Skripten
    // und vor der Physik, after nach dem ganzen Schritt. Leere Funktionen lösen die Haken.
    void set_fixed_hooks(std::function<void()> before, std::function<void()> after) {
        before_physics_ = std::move(before);
        after_step_ = std::move(after);
    }

    // Neue Spielfigur aus prefabs/spielfigur.prefab (ohne Asset-Ordner aus Code) an position.
    world::Entity spawn_figure(DVec3 position, std::string_view name = {}) {
        const asset::AssetId prefab = asset_id("prefabs/spielfigur.prefab");
        world::Entity figure =
            spawn_player(world_, position, prefab != 0 ? prefabs_.get(prefab) : nullptr, prefab);
        if (!name.empty()) {
            figure.set(world::Name{std::string{name}});
        }
        return figure;
    }

    // Spielplatz (R4.2, R4.3): aus der Szene szenen/spielplatz.json, wenn es sie im Asset-Ordner
    // gibt und origin der Ursprung ist, sonst aus Code (build_playground()) mit Figur, Laterne und
    // Klängen. extra_bodies weitere Körper regnen für Messungen herab (--bodies).
    PlaygroundInfo load_playground(DVec3 origin = {}, u32 extra_bodies = 0) {
        PlaygroundInfo info{.spawn = origin};
        const asset::AssetInfo* scene =
            database_ != nullptr ? database_->find_path("szenen/spielplatz.json") : nullptr;
        bool loaded = false;
        if (scene != nullptr && origin == DVec3{0.0, 0.0, 0.0}) {
            loaded = true;
            if (auto result = world::load_scene_file(world_, scene->file, prefabs_.lookup())) {
                log::info("Spielplatz aus {} ({} Einträge)", scene->path, *result);
                info.from_scene = true;
            } else {
                log::error("Spielplatz: {}", result.error());
            }
        }
        if (!loaded) {
            const PlaygroundLayout layout = build_playground(world_, origin, *jobs_);
            (void)spawn_figure(layout.spawn);
            const asset::AssetId lantern = asset_id("prefabs/laterne.prefab");
            if (const world::Prefab* prefab = lantern != 0 ? prefabs_.get(lantern) : nullptr) {
                if (auto placed = world::instantiate(world_, *prefab, lantern)) {
                    world::Transform transform = *placed->get<world::Transform>();
                    transform.position = layout.lantern;
                    placed->set(transform);
                }
            }
            add_playground_audio(world_, layout, database_);
            add_playground_hum(world_, layout, database_);
            info.spawn = layout.spawn;
        }
        (void)world::update_transforms(world_);
        if (const world::Entity player = find_player(world_)) {
            info.spawn = player.get<world::WorldTransform>()->position();
        }
        if (extra_bodies > 0) {
            rain_bodies(world_, info.spawn + DVec3{0.0, 2.0, 14.0}, extra_bodies);
        }
        return info;
    }

    // Szene aus einer Datei: path relativ zum Asset-Ordner (z. B. szenen/spielplatz.json) oder als
    // Pfad im Dateisystem. Liefert die Zahl der Einträge.
    [[nodiscard]] std::expected<usize, std::string> load_scene(std::string_view path) {
        std::string file{path};
        if (const asset::AssetInfo* info =
                database_ != nullptr ? database_->find_path(path) : nullptr) {
            file = info->file;
        }
        auto loaded = world::load_scene_file(world_, file, prefabs_.lookup());
        if (loaded) {
            (void)world::update_transforms(world_);
        }
        return loaded;
    }

    // Ein Bild ohne Fenster (--headless, Server): Transforms, Terrain (mit deterministic wartend
    // auf alle Netze), feste Schritte der Spielschleife, fertig geladene Ressourcen und die
    // Automaten der Animation. Liefert die Zahl der festen Schritte.
    u32 update(f64 dt) {
        NGINE_PROFILE_ZONE("GameWorld::update");
        resources_->update();
        if (scripts_) {
            scripts_->update();
        }
        (void)world::update_transforms(world_);
        (void)world::update_terrains(world_, *jobs_,
                                     {.wait = deterministic_, .wait_for_edits = false});
        const u32 steps = simulation_.advance(dt);
        update_animation(dt);
        return steps;
    }

    // Nach der Simulation des Bildes: Automaten, Posen und Paletten (R4.6). Pause und Zeitlupe
    // der Spielschleife gelten auch hier.
    void update_animation(f64 dt) {
        const f64 scaled = simulation_.paused() ? 0.0 : dt * simulation_.time_scale();
        animation_->update(world_, scaled);
    }

    // Kennung eines Assets im Asset-Ordner, 0 ohne Datenbank oder wenn es fehlt.
    [[nodiscard]] asset::AssetId asset_id(std::string_view path) const {
        const asset::AssetInfo* info = database_ != nullptr ? database_->find_path(path) : nullptr;
        return info != nullptr ? info->id : 0;
    }

    [[nodiscard]] reflect::TypeRegistry& types() noexcept {
        return types_;
    }
    [[nodiscard]] world::World& world() noexcept {
        return world_;
    }
    [[nodiscard]] jobs::JobSystem& jobs() noexcept {
        return *jobs_;
    }
    [[nodiscard]] asset::ResourceManager& resources() noexcept {
        return *resources_;
    }
    [[nodiscard]] const asset::AssetDatabase* database() const noexcept {
        return database_;
    }
    [[nodiscard]] PrefabLibrary& prefabs() noexcept {
        return prefabs_;
    }
    [[nodiscard]] world::Simulation& simulation() noexcept {
        return simulation_;
    }
    [[nodiscard]] physics::PhysicsWorld& physics() noexcept {
        return physics_;
    }
    [[nodiscard]] ModelShapes& shapes() noexcept {
        return shapes_;
    }
    [[nodiscard]] events::EventBus& events() noexcept {
        return events_;
    }
    [[nodiscard]] animation::AnimationSystem& animation() noexcept {
        return *animation_;
    }
    [[nodiscard]] TerrainMaterialLibrary& terrain_materials() noexcept {
        return terrain_materials_;
    }
    // Gibt die Tabelle an die Physik (nach TerrainMaterialLibrary::set() oder poll()).
    void apply_terrain_materials() {
        physics_.set_terrain_materials(terrain_materials_.table());
    }

private:
    struct PlayerSlot {
        u64 figure = 0;
        input::ActionState* actions = nullptr;
        // Graben ohne Skript (R5.4), wie Grabpause im Skript.
        DigTimer dig;
    };

    // Graben der Figuren mit eigenen Aktionen, die kein Skript steuert (R5.4): wie das Skript
    // Spielersteuerung nach den festen Schritten eines Bildes, mit fast auftragen.
    void dig_players(f32 dt) {
        for (PlayerSlot& slot : players_) {
            world::Entity figure = world_.entity(slot.figure);
            if (!figure || !figure.alive() || scripted_player(figure)) {
                continue;
            }
            if (!slot.dig.ready(dt, slot.actions->down("dig"))) {
                continue;
            }
            const auto* input = figure.get<PlayerInput>();
            const auto target = figure_dig_target(world_, figure, physics_);
            if (input != nullptr && target) {
                dig(*target, *input, slot.actions->down("fast"));
                slot.dig.used();
            }
        }
    }

    reflect::TypeRegistry types_;
    world::World world_;
    std::unique_ptr<jobs::JobSystem> own_jobs_;
    jobs::JobSystem* jobs_;
    // Nach den Jobs angelegt, deshalb vor ihnen zerstört: Laufende Ladevorgänge müssen fertig sein.
    std::unique_ptr<asset::ResourceManager> resources_;
    PrefabLibrary prefabs_;
    world::Simulation simulation_;
    ModelShapes shapes_;
    physics::PhysicsWorld physics_;
    const asset::AssetDatabase* database_ = nullptr;
    TerrainMaterialLibrary terrain_materials_;
    bool deterministic_ = false;
    input::ActionState* actions_ = nullptr;
    std::vector<PlayerSlot> players_;
    std::function<void()> before_physics_;
    std::function<void()> after_step_;
    // Ereignisse der Physik für die Skripte; nach Welt und Physik angelegt, damit die Skripte vor
    // ihnen gehen.
    events::EventBus events_;
    // Hält Rigs und Automaten aus resources_; die Skripte verweisen darauf.
    std::unique_ptr<animation::AnimationSystem> animation_;
    std::unique_ptr<script::ScriptSystem> scripts_;
};

} // namespace app
