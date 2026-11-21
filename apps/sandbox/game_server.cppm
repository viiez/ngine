module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Dedizierter Server mit Spielwelt (R5.1, NET-02): ohne Fenster und GPU rechnet er über
// app::GameWorld dieselbe Welt wie Sandbox, Editor und --headless, standardmäßig den Spielplatz mit
// Terrain, Physik, Skripten (aus dem Cache) und Automaten der Animation. Ein Bild ist genau ein
// fester Schritt zu 1/60 s; hinkt er hinterher, holt er ohne Pause auf. So hängt das Ergebnis
// nicht von der Uhr ab, und ohne Clients rechnet er bitgleich wie --headless --playground.
//
// Clients verbinden sich über ngine.replication. Der erste übernimmt die Figur aus der Szene, jeder
// weitere bekommt eine neue aus prefabs/spielfigur.prefab neben dem Startpunkt; geht er, endet
// seine Figur (die aus der Szene bleibt stehen). Jeder Client schickt pro Bild die Werte seiner
// Aktionen und seinen Blick; auf dem Server liest das Skript Spielersteuerung seiner Figur genau
// diese (ohne .NET die Steuerung in C++).
//
// Seit R5.2 repliziert der Server die ganze Welt (ADR 0052): jede Entity mit Komponenten, die die
// Reflection als repliziert markiert, als Delta gegen den letzten bestätigten Stand jedes Clients,
// aus 150 m um seine Figur und höchstens 64 KB/s. Terrain und Sonne sehen alle. Die Parameter der
// Automaten gehen mit (AnimatorParameters), damit fremde Figuren auf Clients laufen. Deltas des
// Terrains folgen in R5.4.
//
// Clients ohne Fenster (--client --headless) und Bots (--bots) stehen ebenfalls hier; die Sandbox
// mit Fenster als Client steht in main.cpp.
//
// Bis R4.6 rechnete --server die Kugeln der Szenen-Demo aus D3 (TD-54, TD-63).
export module sandbox.game_server;

import ngine.core;
import ngine.animation;
import ngine.asset;
import ngine.input;
import ngine.math;
import ngine.net;
import ngine.physics;
import ngine.platform;
import ngine.profile;
import ngine.reflect;
import ngine.render;
import ngine.replication;
import ngine.script;
import ngine.world;
import app.game_world;
import app.physics;
import app.player;
import app.scripting;
import sandbox.net_client;
import sandbox.playground;

namespace sandbox {

using namespace ngine;

// Registry, wie Clients sie brauchen: dieselben Typen wie die Spielwelt (gleiche Reihenfolge,
// gleiches Schema) und Replicated.
export reflect::TypeRegistry make_client_types() {
    reflect::TypeRegistry types = app::make_game_types();
    replication::register_types(types);
    return types;
}

// Komponenten, deren Entities jeder Client sieht, egal wie weit weg.
export std::vector<const reflect::TypeInfo*> global_components(const reflect::TypeRegistry& types) {
    return {&types.get<world::Terrain>(), &types.get<render::DirectionalLight>()};
}

export struct GameServerOptions {
    // Ohne Angabe läuft der Server, bis man ihn beendet.
    std::optional<u64> frames;
    std::optional<u16> port;
    // Szene relativ zum Asset-Ordner oder als Pfad; ohne Angabe der Spielplatz.
    std::optional<std::string> scene;
    // Die Figur der Szene läuft die feste Spur von --headless --playground (wie ein Client).
    bool track = false;
    bool scripts = true;
    // Weitere Körper auf dem Spielplatz, z. B. 500 für die Messung der Bandbreite (--bodies).
    u32 bodies = 0;
    const asset::AssetDatabase* database = nullptr;
    const asset::AssetCache* cache = nullptr;
};

namespace {

// Taktet Bilder mit 60 Hz. Wer mehr als eine Viertelsekunde zurückliegt (Laden, Haltepunkt),
// fängt neu an, statt hunderte Bilder nachzuholen.
class TickClock {
public:
    void wait() {
        next_ += std::chrono::microseconds{16'667};
        const auto now = std::chrono::steady_clock::now();
        if (now > next_ + std::chrono::milliseconds{250}) {
            next_ = now;
            ++resets_;
        } else if (now < next_) {
            std::this_thread::sleep_until(next_);
        }
        profile::frame_mark();
    }
    [[nodiscard]] u64 resets() const noexcept {
        return resets_;
    }

private:
    std::chrono::steady_clock::time_point next_ = std::chrono::steady_clock::now();
    u64 resets_ = 0;
};

std::unique_ptr<net::Transport> make_transport() {
    auto transport = net::Transport::create();
    if (!transport) {
        log::error("{}", transport.error());
        return nullptr;
    }
    return std::move(*transport);
}

// Ein verbundener Spieler: seine Figur und die Aktionen, die er zuletzt geschickt hat.
struct Player {
    u64 figure = 0;
    // Figur neu angelegt (endet mit ihm) oder aus der Szene übernommen (bleibt).
    bool spawned = false;
    std::unique_ptr<input::ActionState> actions;
    // Letzte Werte und die Flanken aller Nachrichten seit dem letzten Bild.
    std::vector<input::ActionValue> latest;
    bool fresh = false;
    // Blick, den der Client zuletzt geschickt hat (R5.2); ersetzt dann die Mausbewegung.
    std::optional<replication::ViewAngles> view;
    u64 messages = 0;
    u64 rejected = 0;
};

class GameServer {
public:
    GameServer(app::GameWorld& game, replication::Server& server, world::Entity scene_figure,
               math::DVec3 spawn)
        : game_{&game}, server_{&server}, scene_figure_{scene_figure}, spawn_{spawn},
          map_hash_{input::ActionMap::standard().hash()},
          look_{input::ActionMap::standard().find("look")},
          dig_{input::ActionMap::standard().find("dig")},
          throw_{input::ActionMap::standard().find("throw")} {}

    // Die Figur der Szene gehört der Spur (--track) und keinem Client.
    void reserve_scene_figure() {
        scene_figure_reserved_ = true;
    }

    // Neue Clients bekommen eine Figur, getrennte geben sie ab.
    void sync_players(std::span<const net::ConnectionId> clients) {
        for (const net::ConnectionId client : clients) {
            if (!players_.contains(client)) {
                join(client);
            }
        }
        for (auto it = players_.begin(); it != players_.end();) {
            if (std::ranges::find(clients, it->first) == clients.end()) {
                leave(it->first, it->second);
                it = players_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void handle(const replication::Received& message) {
        const auto found = players_.find(message.connection);
        if (found == players_.end()) {
            return;
        }
        Player& player = found->second;
        const auto kind = replication::game_message(message.data);
        if (kind == replication::GameMessage::View) {
            if (auto view = replication::decode_view(message.data)) {
                player.view = *view;
            } else if (player.rejected++ == 0) {
                log::warn("Client {}: {}", message.connection, view.error());
            }
            return;
        }
        if (kind != replication::GameMessage::Actions) {
            return;
        }
        auto values = replication::decode_actions(message.data, map_hash_);
        if (!values) {
            if (player.rejected++ == 0) {
                log::warn("Client {}: {}", message.connection, values.error());
            }
            return;
        }
        // Mehrere Nachrichten in einem Bild: letzte Werte, aber keine Flanke geht verloren.
        if (player.fresh && player.latest.size() == values->size()) {
            for (usize i = 0; i < values->size(); ++i) {
                (*values)[i].pressed = (*values)[i].pressed || player.latest[i].pressed;
                (*values)[i].released = (*values)[i].released || player.latest[i].released;
            }
        }
        player.latest = std::move(*values);
        player.fresh = true;
        ++player.messages;
    }

    // Vor dem Schritt: die Werte jedes Spielers für dieses Bild. Ohne neue Nachricht bleiben die
    // Werte stehen, Flanken zählen nur einmal.
    void apply_inputs() {
        for (auto& [client, player] : players_) {
            if (!player.fresh) {
                for (input::ActionValue& value : player.latest) {
                    value.pressed = false;
                    value.released = false;
                }
            }
            // Dreht der Client die Kamera selbst, gilt sein Blick statt der Mausbewegung, damit
            // verlorene Pakete den Blick nicht verstellen.
            if (player.view) {
                if (look_ && *look_ < player.latest.size()) {
                    player.latest[*look_].x = 0.0f;
                    player.latest[*look_].y = 0.0f;
                }
                if (auto* view = game_->world().entity(player.figure).get_mut<app::PlayerInput>()) {
                    view->yaw = player.view->yaw;
                    view->pitch = std::clamp(player.view->pitch, -1.55f, 1.55f);
                }
            }
            // Löcher im Terrain kämen nicht bei den Clients an (Terrain-Deltas übers Netz folgen
            // in R5.4), Figuren und Kisten fielen für sie durch den Boden: bis dahin kein Graben.
            if (dig_ && *dig_ < player.latest.size() && player.latest[*dig_].down) {
                player.latest[*dig_] = {};
                if (!dig_warned_) {
                    log::info("Server: Graben von Clients ist bis R5.4 abgeschaltet");
                    dig_warned_ = true;
                }
            }
            if (throw_ && *throw_ < player.latest.size() && player.latest[*throw_].pressed) {
                throw_ball(player.figure);
            }
            player.actions->apply(player.latest);
            player.fresh = false;
        }
    }

    // Kugel aus dem Auge der Figur in Blickrichtung, wie ModelScene::throw_ball() lokal.
    void throw_ball(u64 figure_id) {
        world::World& world = game_->world();
        const world::Entity figure = world.entity(figure_id);
        const auto* transform = figure ? figure.get<world::WorldTransform>() : nullptr;
        const auto* input = figure ? figure.get<app::PlayerInput>() : nullptr;
        if (transform == nullptr || input == nullptr) {
            return;
        }
        f32 eye = input->eye_height;
        if (const auto* motion = figure.get<physics::CharacterMotion>();
            motion != nullptr && motion->crouched) {
            eye *= 0.6f;
        }
        const math::Vec3 forward = app::view_rotation(*input) * math::Vec3{0.0f, 0.0f, -1.0f};
        const math::DVec3 origin =
            transform->position() + math::DVec3{0.0, static_cast<f64>(eye), 0.0};
        world::Entity ball =
            app::add_ball(world, origin + math::to_f64(forward * 0.8f), forward * 18.0f);
        ball.set(world::Name{"Wurf"});
    }

    // Mittelpunkt der Relevanz jedes Clients: seine Figur.
    void update_focus() {
        for (const auto& [client, player] : players_) {
            const world::Entity figure = game_->world().entity(player.figure);
            const auto* transform = figure ? figure.get<world::WorldTransform>() : nullptr;
            server_->set_focus(client, transform != nullptr ? std::optional{transform->position()}
                                                            : std::nullopt);
        }
    }

    [[nodiscard]] usize player_count() const noexcept {
        return players_.size();
    }
    [[nodiscard]] u64 figure_of(net::ConnectionId client) const {
        const auto found = players_.find(client);
        return found != players_.end() ? found->second.figure : 0;
    }

    // Eine Zeile pro Spieler für den Statusbericht.
    void report() const {
        for (const auto& [client, player] : players_) {
            const world::Entity figure = game_->world().entity(player.figure);
            const auto* transform = figure ? figure.get<world::Transform>() : nullptr;
            const math::DVec3 at =
                transform != nullptr ? transform->position - spawn_ : math::DVec3{};
            const replication::EncoderClientStats stats = server_->stats(client);
            log::info("  Client {}: Figur {} ({:.1f} m vom Start), {} Eingaben, {} abgelehnt; "
                      "{:.1f} KB/s, Schnappschuss {} B, {} Entities, {} warten, {} ohne Basis",
                      client, figure ? figure.name() : std::string_view{"fehlt"}, math::length(at),
                      player.messages, player.rejected,
                      static_cast<f64>(stats.bytes_per_second) / 1024.0, stats.last_bytes,
                      stats.entities, stats.waiting, stats.full);
        }
    }

private:
    void join(net::ConnectionId client) {
        Player player;
        world::World& world = game_->world();
        if (!scene_figure_reserved_ && scene_figure_ && !scene_figure_taken_ &&
            world.alive(scene_figure_.id())) {
            player.figure = scene_figure_.id();
            scene_figure_taken_ = true;
        } else {
            // Neben dem Startpunkt auf einem Kreis, damit sich die Figuren nicht stapeln.
            const f64 angle = static_cast<f64>(++spawned_) * 2.399963;
            const math::DVec3 position =
                spawn_ + math::DVec3{std::cos(angle) * 2.5, 0.5, std::sin(angle) * 2.5};
            const world::Entity figure =
                game_->spawn_figure(position, std::format("Spieler {}", client));
            player.figure = figure.id();
            player.spawned = true;
        }
        player.actions = std::make_unique<input::ActionState>(input::ActionMap::standard());
        game_->bind_player(world.entity(player.figure), *player.actions);
        (void)server_->send(client, replication::encode_possess(player.figure),
                            net::Delivery::Reliable);
        log::info("Client {} steuert {} ({})", client, world.entity(player.figure).name(),
                  player.spawned ? "neu" : "aus der Szene");
        players_.emplace(client, std::move(player));
    }

    void leave(net::ConnectionId client, Player& player) {
        world::World& world = game_->world();
        const world::Entity figure = world.entity(player.figure);
        game_->unbind_player(figure);
        if (player.spawned) {
            if (world.alive(player.figure)) {
                world.destroy(player.figure);
            }
        } else {
            scene_figure_taken_ = false;
            // Die Figur bleibt stehen, wo sie war.
            if (auto* motion = world.entity(player.figure).get_mut<physics::CharacterMotion>()) {
                motion->move = {};
                motion->run = false;
                motion->crouch = false;
                motion->jump = false;
            }
        }
        log::info("Client {} hat das Spiel verlassen ({} Eingaben)", client, player.messages);
    }

    app::GameWorld* game_;
    replication::Server* server_;
    world::Entity scene_figure_;
    bool scene_figure_reserved_ = false;
    bool scene_figure_taken_ = false;
    math::DVec3 spawn_;
    u32 map_hash_;
    std::optional<usize> look_;
    std::optional<usize> dig_;
    std::optional<usize> throw_;
    bool dig_warned_ = false;
    u32 spawned_ = 0;
    std::map<net::ConnectionId, Player> players_;
};

} // namespace

export int run_game_server(const GameServerOptions& options) {
    // Vor der Welt angelegt, damit sie nach deren Skripten endet.
    const std::unique_ptr<script::ScriptRuntime> runtime =
        options.scripts ? app::start_script_runtime() : nullptr;
    app::GameWorld game{
        {.database = options.database, .cache = options.cache, .deterministic = true}};
    world::World& world = game.world();
    game.animation().set_rendering(false);
    // Parameter der Automaten für die Clients (R5.2).
    game.animation().set_parameter_sync(animation::ParameterSync::Publish);
    // Skripte aus dem Cache; mit .NET-SDK baut der Server sie vorher, wenn sie veraltet sind.
    // Gewartet wird, damit die Szene ihre Skript-Komponenten kennt.
    if (runtime) {
        game.enable_scripts(*runtime, nullptr, {.watch = false, .wait = true});
    }
    const auto loading = std::chrono::steady_clock::now();
    math::DVec3 spawn{};
    if (options.scene) {
        if (auto loaded = game.load_scene(*options.scene)) {
            log::info("Server: Szene {} ({} Einträge)", *options.scene, *loaded);
        } else {
            log::error("Server: Szene {}: {}", *options.scene, loaded.error());
            return 1;
        }
        if (const world::Entity player = app::find_player(world)) {
            spawn = player.get<world::WorldTransform>()->position();
        }
    } else {
        spawn = game.load_playground({}, options.bodies).spawn;
    }
    world::Entity scene_figure = app::find_player(world);
    replication::register_types(game.types());
    const replication::Schema schema{game.types()};

    auto transport = make_transport();
    if (!transport) {
        return 1;
    }
    replication::Server server{*transport, world, schema};
    server.set_global(global_components(game.types()));
    GameServer players{game, server, scene_figure, spawn};
    std::optional<PlaygroundTrack> track;
    if (options.track && scene_figure) {
        players.reserve_scene_figure();
        track.emplace(game, scene_figure);
    }
    // Gelauscht wird erst nach dem ersten Bild: Es wartet auf Terrain und Kollision um die Figuren
    // (deterministisch, im Debug-Build mehrere Sekunden), und ein Client, der in dieser Zeit
    // verbindet, bekäme keine Antwort.
    std::optional<u16> port;
    const auto listen = [&]() -> bool {
        const auto bound = transport->listen(options.port.value_or(default_game_port));
        if (!bound) {
            log::error("{}", bound.error());
            return false;
        }
        port = *bound;
        const std::chrono::duration<f64, std::milli> load_time =
            std::chrono::steady_clock::now() - loading;
        log::info("Server bereit auf Port {}: {} Entities, {} Körper, {} replizierte Komponenten, "
                  "Skripte {}, Figur der Szene {}{}, geladen in {:.0f} ms",
                  *port, world.entity_count(), game.physics().body_count(), schema.size(),
                  game.scripts_running() ? "laufen" : "aus",
                  scene_figure ? scene_figure.name() : std::string_view{"fehlt"},
                  track ? " (läuft die Spur)" : "", load_time.count());
        // Sofort sichtbar, auch wenn stdout in eine Datei geht: Skripte warten auf diese Zeile.
        log::flush();
        return true;
    };
    if (options.frames && *options.frames == 0 && !listen()) {
        return 1;
    }

    std::vector<replication::Received> received;
    TickClock clock;
    f64 step_total = 0.0;
    f64 step_max = 0.0;
    u64 step_count = 0;
    usize snapshot_bytes = 0;
    for (u64 frame = 0; !options.frames || frame < *options.frames; ++frame) {
        NGINE_PROFILE_ZONE("Server::frame");
        const auto start = std::chrono::steady_clock::now();
        received.clear();
        if (port) {
            server.poll(received);
        }
        players.sync_players(server.clients());
        for (const replication::Received& message : received) {
            players.handle(message);
        }
        players.apply_inputs();
        if (track) {
            track->apply(frame);
        }
        game.steer_players();
        (void)game.update(1.0 / 60.0);
        // Schnappschüsse mit 30 Hz.
        if (frame % 2 == 0) {
            players.update_focus();
            server.send_snapshot();
            snapshot_bytes = std::max(snapshot_bytes, server.last_snapshot_size());
        }
        const f64 ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        step_total += ms;
        step_max = std::max(step_max, ms);
        ++step_count;
        if (frame % 300 == 299) {
            const physics::StepStats& stats = game.physics().stats();
            log::info("Server: fester Schritt {}, {} Client(s), {} Entities, {} Körper ({} wach), "
                      "{} Figuren, {} repliziert; Bild im Mittel {:.2f} ms, höchstens {:.2f} ms; "
                      "Schnappschuss bis {} Bytes",
                      game.simulation().time().tick, players.player_count(), world.entity_count(),
                      game.physics().body_count(), stats.active_bodies, stats.characters,
                      server.entity_count(), step_total / static_cast<f64>(step_count), step_max,
                      snapshot_bytes);
            players.report();
            step_total = 0.0;
            step_max = 0.0;
            step_count = 0;
            snapshot_bytes = 0;
        }
        if (!port && !listen()) {
            return 1;
        }
        // Mit --track rechnet der Server so schnell es geht (Vergleich mit --headless).
        if (!track) {
            clock.wait();
        }
    }
    log::info("Server: {} feste Schritte, {} Mal neu getaktet", game.simulation().time().tick,
              clock.resets());
    log::info("Prüfsumme der Lagen: {:016x}", physics::checksum(world));
    return 0;
}

// Client ohne Fenster (R5.1): verbindet, schickt pro Bild die Aktionen der festen Spur und
// meldet, wo seine Figur auf dem Server steht. Fehlschlag, wenn bis zum Ende keine Figur kam oder
// sie sich nicht bewegt hat. Seit R5.2 mit der ganzen Welt als Delta.
export int run_game_client(const std::string& address, u64 frames) {
    reflect::TypeRegistry types = make_client_types();
    const replication::Schema schema{types};
    world::World world{types};
    auto connected = NetClient::connect(world, schema, address);
    if (!connected) {
        log::error("{}", connected.error());
        return 1;
    }
    NetClient& net = **connected;
    input::ActionState actions{input::ActionMap::standard()};
    const u32 map_hash = actions.map().hash();
    TickClock clock;
    std::optional<math::DVec3> first;
    math::DVec3 last{};
    u64 track_frame = 0;
    for (u64 frame = 0; frame < frames; ++frame) {
        if (auto result = net.poll(); !result) {
            log::error("{}", result.error());
            return 1;
        }
        if (net.disconnected()) {
            log::error("Keine Verbindung zum Server");
            return 1;
        }
        if (net.ready()) {
            // Die Spur beginnt, sobald die Figur feststeht.
            actions.begin_frame();
            for (const platform::Event& event : scripted_events(track_frame++)) {
                actions.handle(event);
            }
            actions.end_frame(1.0f / 60.0f);
            net.send_input(map_hash, actions.values());
            const u64 local = net.figure();
            if (const auto* transform =
                    local != 0 ? world.entity(local).get<world::Transform>() : nullptr) {
                if (!first) {
                    first = transform->position;
                }
                last = transform->position;
            }
        }
        if (frame % 60 == 59 && first) {
            log::info("Client: Figur {:.2f} m vom ersten Stand, {} Entities gespiegelt, Tick {}",
                      math::length(last - *first), net.client().mirror().entity_count(),
                      net.client().mirror().last_tick());
        }
        if (frame % 180 == 179) {
            log::info("{}", net.report());
        }
        clock.wait();
    }
    log::info("Client: {} Schnappschüsse angewendet", net.client().snapshots_applied());
    log::info("{}", net.report());
    net.disconnect();
    // Kurz weiterpollen, damit die Trennung beim Server ankommt.
    for (int i = 0; i < 10; ++i) {
        (void)net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (net.figure_on_server() == 0 || !first) {
        log::error("Keine Figur vom Server bekommen");
        return 1;
    }
    const f64 moved = math::length(last - *first);
    if (moved < 1.0) {
        log::error("Die Figur hat sich auf dem Server nicht bewegt ({:.2f} m)", moved);
        return 1;
    }
    log::info("Client: Figur ist auf dem Server {:.1f} m gelaufen", moved);
    return 0;
}

namespace {

// Ein Bot: eigene Welt, eigene Verbindung, läuft die feste Spur versetzt und im Kreis.
struct Bot {
    reflect::TypeRegistry types = make_client_types();
    std::unique_ptr<world::World> world = std::make_unique<world::World>(types);
    std::unique_ptr<replication::Schema> schema = std::make_unique<replication::Schema>(types);
    std::unique_ptr<NetClient> net;
    input::ActionState actions{input::ActionMap::standard()};
    u64 offset = 0;
    u64 track_frame = 0;
    // Bytes der Schnappschüsse pro Sekunde, einmal pro Sekunde gemessen.
    std::vector<usize> rates;
};

} // namespace

// count Clients ohne Fenster in einem Prozess (R5.2): messen Bandbreite pro Client. Fehlschlag,
// wenn einer keine Figur bekommt oder getrennt wird.
export int run_bots(const std::string& address, u32 count, u64 frames) {
    std::vector<std::unique_ptr<Bot>> bots;
    for (u32 i = 0; i < count; ++i) {
        auto bot = std::make_unique<Bot>();
        auto connected = NetClient::connect(*bot->world, *bot->schema, address);
        if (!connected) {
            log::error("Bot {}: {}", i + 1, connected.error());
            return 1;
        }
        bot->net = std::move(*connected);
        bot->offset = static_cast<u64>(i) * 97;
        bots.push_back(std::move(bot));
    }
    const u32 map_hash = input::ActionMap::standard().hash();
    TickClock clock;
    for (u64 frame = 0; frame < frames; ++frame) {
        for (usize i = 0; i < bots.size(); ++i) {
            Bot& bot = *bots[i];
            if (auto result = bot.net->poll(); !result) {
                log::error("Bot {}: {}", i + 1, result.error());
                return 1;
            }
            if (bot.net->disconnected()) {
                log::error("Bot {}: keine Verbindung zum Server", i + 1);
                return 1;
            }
            if (!bot.net->ready()) {
                continue;
            }
            bot.actions.begin_frame();
            for (const platform::Event& event :
                 scripted_events((bot.track_frame++ + bot.offset) % 600)) {
                bot.actions.handle(event);
            }
            bot.actions.end_frame(1.0f / 60.0f);
            bot.net->send_input(map_hash, bot.actions.values());
            if (frame % 60 == 59) {
                bot.rates.push_back(bot.net->client().stats().snapshot_bytes_per_second);
            }
        }
        if (frame % 300 == 299) {
            log::info("Bot 1: {}", bots.front()->net->report());
        }
        clock.wait();
    }
    usize total = 0;
    usize samples = 0;
    usize peak = 0;
    bool ok = true;
    for (usize i = 0; i < bots.size(); ++i) {
        Bot& bot = *bots[i];
        // Die ersten Sekunden (Beitritt) zählen für den Höchstwert, nicht für den Mittelwert.
        usize sum = 0;
        usize n = 0;
        for (usize s = 0; s < bot.rates.size(); ++s) {
            peak = std::max(peak, bot.rates[s]);
            if (s >= 2) {
                sum += bot.rates[s];
                ++n;
            }
        }
        total += sum;
        samples += n;
        log::info("Bot {}: Figur {}, {} Schnappschüsse, {} Entities, im Mittel {:.1f} KB/s; {}",
                  i + 1, bot.net->figure_on_server(), bot.net->client().snapshots_applied(),
                  bot.net->client().mirror().entity_count(),
                  n > 0 ? static_cast<f64>(sum) / static_cast<f64>(n) / 1024.0 : 0.0,
                  bot.net->report());
        if (!bot.net->ready()) {
            log::error("Bot {} hat keine Figur bekommen", i + 1);
            ok = false;
        }
        bot.net->disconnect();
    }
    for (int i = 0; i < 10; ++i) {
        for (const auto& bot : bots) {
            (void)bot->net->poll();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    log::info("Bots: {} Clients, Schnappschüsse im Mittel {:.1f} KB/s, höchstens {:.1f} KB/s pro "
              "Client (Budget 64 KB/s)",
              bots.size(),
              samples > 0 ? static_cast<f64>(total) / static_cast<f64>(samples) / 1024.0 : 0.0,
              static_cast<f64>(peak) / 1024.0);
    return ok ? 0 : 1;
}

} // namespace sandbox
