module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Server mit Spielwelt als Baustein (R5.5; bis R5.4 in sandbox/game_server.cppm). Der dedizierte
// Server (Sandbox --server), der Listen-Server (Sandbox --host, Editor) und die Tests nutzen ihn.
//
// GameServer verwaltet die Spieler: Der erste Client übernimmt die Figur aus der Szene, jeder
// weitere bekommt eine neue aus prefabs/spielfigur.prefab neben dem Startpunkt; geht er, endet
// seine Figur (die aus der Szene bleibt stehen). Jeder Client schickt pro festem Schritt einen
// nummerierten Befehl mit Aktionen und Blick (R5.3); der Server prüft ihn und arbeitet pro Bild
// genau einen aus der Warteschlange ab. Mit jedem Schnappschuss bekommt jeder Client den Stand
// seiner Figur nach seinem letzten Befehl (R5.3) und die geänderten Deltas des Terrains (R5.4).
//
// ServerSession setzt Transport, Replikation (ADR 0052) und GameServer über einer app::GameWorld
// zusammen; ein Bild des Servers ist genau ein fester Schritt zu 1/60 s. ListenServer besitzt
// dazu die Welt und taktet die Bilder des Servers aus der Zeit des Hosts (R5.5, NET-03); der Host
// verbindet sich über eine Verbindung im Speicher wie jeder andere Client.
export module app.game_server;

import ngine.core;
import ngine.animation;
import ngine.asset;
import ngine.audio;
import ngine.filesystem;
import ngine.input;
import ngine.jobs;
import ngine.math;
import ngine.net;
import ngine.physics;
import ngine.profile;
import ngine.reflect;
import ngine.render;
import ngine.replication;
import ngine.script;
import ngine.world;
import app.game_world;
import app.net_client;
import app.physics;
import app.player;
import app.prediction;

namespace app {

using namespace ngine;

// Registry, wie Clients sie brauchen: dieselben Typen wie die Spielwelt (gleiche Reihenfolge,
// gleiches Schema) und Replicated.
export reflect::TypeRegistry make_client_types() {
    reflect::TypeRegistry types = make_game_types();
    replication::register_types(types);
    return types;
}

// Skripte für einen Client ohne Spielwelt (Bots, R5.5): Seine Registry braucht dieselben
// Skript-Typen wie die des Servers, sonst passt das Schema nicht, sobald Skripte repliziert werden.
// Die Skripte laufen nicht; das System muss die Registry überleben lassen (vor ihr enden).
// nullptr ohne Laufzeit oder Projekt.
export std::unique_ptr<script::ScriptSystem>
make_client_scripts(script::ScriptRuntime* runtime, reflect::TypeRegistry& types,
                    const asset::AssetDatabase* database) {
    if (runtime == nullptr || database == nullptr) {
        return nullptr;
    }
    const std::string project = filesystem::join(database->root(), "skripte/Skripte.csproj");
    if (!filesystem::exists(project)) {
        return nullptr;
    }
    auto scripts = std::make_unique<script::ScriptSystem>(
        *runtime, types,
        script::ScriptSystemDesc{.project = project,
                                 .cache_dir = filesystem::join(database->root(), ".cache/skripte"),
                                 .build = false,
                                 .watch = false,
                                 .role = script::ScriptRole::Client});
    scripts->start(true);
    return scripts;
}

// Komponenten, deren Entities jeder Client sieht, egal wie weit weg.
export std::vector<const reflect::TypeInfo*> global_components(const reflect::TypeRegistry& types) {
    return {&types.get<world::Terrain>(), &types.get<render::DirectionalLight>()};
}

struct ConnectedPlayer {
    u64 figure = 0;
    // Figur neu angelegt (endet mit ihm) oder aus der Szene übernommen (bleibt).
    bool spawned = false;
    std::unique_ptr<input::ActionState> actions;
    // Befehle pro festem Schritt, einer pro Bild.
    replication::CommandQueue commands;
    // Werte des Befehls dieses Bildes.
    std::vector<input::ActionValue> latest;
    // Ob in diesem Bild ein Befehl galt (dann geht der Stand der Figur an den Client).
    bool processed = false;
    // Seit der letzten Nachricht des Terrains galt ein Befehl mit dig: Der Client wartet auf die
    // Bestätigung seiner vorhergesagten Pinsel (R5.4).
    bool dug = false;
    // Schritte, seit die Figur zuletzt einen beweglichen Körper berührte, rückwärts gezählt
    // (Einordnen von Korrekturen beim Client: Berührung in den letzten 15 Schritten).
    u32 touch_steps = 0;
    u64 messages = 0;
    // Nachrichten, die nicht zu lesen waren, und Befehle mit Werten außerhalb der Grenzen.
    u64 rejected = 0;
    u64 limited = 0;
};

export class GameServer {
public:
    GameServer(GameWorld& game, replication::Server& server, world::Entity scene_figure,
               math::DVec3 spawn)
        : game_{&game}, server_{&server}, scene_figure_{scene_figure}, spawn_{spawn},
          map_{input::ActionMap::standard()}, map_hash_{map_.hash()}, look_{map_.find("look")},
          dig_{map_.find("dig")}, throw_{map_.find("throw")} {}

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
        ConnectedPlayer& player = found->second;
        if (replication::game_message(message.data) != replication::GameMessage::Commands) {
            return;
        }
        auto commands = replication::decode_commands(message.data, map_hash_);
        if (!commands) {
            if (player.rejected++ == 0) {
                log::warn("Client {}: {}", message.connection, commands.error());
            }
            return;
        }
        ++player.messages;
        for (replication::Command& command : *commands) {
            // Prüfen (R5.3): Länge von move, Tasten, Blick. Die Bewegung selbst rechnet der Server.
            if (replication::limit_command(command, map_, max_pitch) > 0 && player.limited++ == 0) {
                log::warn("Client {}: Befehl {} außerhalb der Grenzen, begrenzt",
                          message.connection, command.number);
            }
            player.commands.push(std::move(command));
        }
    }

    // Vor dem Schritt: der nächste Befehl jedes Spielers. Vor dem ersten Befehl gilt keine
    // Eingabe; fehlt einer, wiederholt die Warteschlange den letzten ohne Flanken.
    void apply_inputs() {
        for (auto& [client, player] : players_) {
            player.processed = false;
            auto command = player.commands.next();
            if (!command) {
                for (input::ActionValue& value : player.latest) {
                    value.pressed = false;
                    value.released = false;
                }
                player.actions->apply(player.latest);
                continue;
            }
            player.processed = true;
            player.latest = std::move(command->actions);
            // Der Blick steht im Befehl; Mausbewegung gibt es nicht.
            if (look_ && *look_ < player.latest.size()) {
                player.latest[*look_].x = 0.0f;
                player.latest[*look_].y = 0.0f;
            }
            if (auto* view = game_->world().entity(player.figure).get_mut<PlayerInput>()) {
                view->yaw = command->yaw;
                view->pitch = std::clamp(command->pitch, -max_pitch, max_pitch);
            }
            // Graben (R5.4): Ziel, Reichweite und Takt bestimmt der Server aus seiner Figur.
            if (dig_ && *dig_ < player.latest.size() && player.latest[*dig_].down) {
                player.dug = true;
            }
            if (throw_ && *throw_ < player.latest.size() && player.latest[*throw_].pressed) {
                throw_ball(player.figure);
            }
            player.actions->apply(player.latest);
        }
    }

    // Nach jedem Schritt: ob die Figuren gerade bewegliche Körper berühren.
    void track_contacts() {
        for (auto& [client, player] : players_) {
            const auto snapshot = game_->physics().character_snapshot(player.figure);
            if (snapshot && snapshot->touching) {
                player.touch_steps = 15;
            } else if (player.touch_steps > 0) {
                --player.touch_steps;
            }
        }
    }

    // Nach dem Schritt: jedem Spieler der Stand seiner Figur nach seinem letzten Befehl (R5.3).
    void send_owner_states() {
        for (auto& [client, player] : players_) {
            if (player.commands.last() == 0) {
                continue;
            }
            const auto snapshot = game_->physics().character_snapshot(player.figure);
            if (!snapshot) {
                continue;
            }
            const replication::OwnerState state{
                .command = player.commands.last(),
                .queued = static_cast<u8>(std::min<usize>(player.commands.size(), 255)),
                .position = snapshot->position,
                .velocity = snapshot->velocity,
                .ground = static_cast<u8>(snapshot->ground),
                .crouched = snapshot->crouched,
                .touching = player.touch_steps > 0};
            (void)server_->send(client, replication::encode_owner(state),
                                net::Delivery::Unreliable);
        }
    }

    // Mit jedem Schnappschuss (R5.4): Stand aller Terrains an den Sender, dann jedem Client die
    // geänderten Deltas mit der Bestätigung seines letzten Befehls.
    void send_terrain() {
        world::World& world = game_->world();
        std::vector<u64> terrains;
        world.each<const world::Terrain>([&](world::Entity entity, const world::Terrain&) {
            auto deltas = world::terrain_deltas(entity);
            if (!deltas) {
                // Noch ohne Laufzeitdaten; kommt mit dem nächsten update_terrains().
                return;
            }
            terrains.push_back(entity.id());
            terrain_.update_terrain(entity.id(), std::move(deltas),
                                    world::terrain_world(entity, math::DVec3{0.0, 0.0, 0.0}));
        });
        terrain_.retain_terrains(terrains);
        for (auto& [client, player] : players_) {
            const world::Entity figure = world.entity(player.figure);
            const auto* transform = figure ? figure.get<world::WorldTransform>() : nullptr;
            terrain_.set_focus(client, transform != nullptr ? std::optional{transform->position()}
                                                            : std::nullopt);
            for (const auto& message :
                 terrain_.encode(client, player.commands.last(), player.dug)) {
                (void)server_->send(client, message, net::Delivery::Reliable);
            }
            player.dug = false;
        }
    }

    [[nodiscard]] const replication::TerrainSender& terrain() const noexcept {
        return terrain_;
    }

    // Kugel aus dem Auge der Figur in Blickrichtung, wie ModelScene::throw_ball() lokal.
    void throw_ball(u64 figure_id) {
        world::World& world = game_->world();
        const world::Entity figure = world.entity(figure_id);
        const auto* transform = figure ? figure.get<world::WorldTransform>() : nullptr;
        const auto* input = figure ? figure.get<PlayerInput>() : nullptr;
        if (transform == nullptr || input == nullptr) {
            return;
        }
        f32 eye = input->eye_height;
        if (const auto* motion = figure.get<physics::CharacterMotion>();
            motion != nullptr && motion->crouched) {
            eye *= 0.6f;
        }
        const math::Vec3 forward = view_rotation(*input) * math::Vec3{0.0f, 0.0f, -1.0f};
        const math::DVec3 origin =
            transform->position() + math::DVec3{0.0, static_cast<f64>(eye), 0.0};
        world::Entity ball =
            add_ball(world, origin + math::to_f64(forward * 0.8f), forward * 18.0f);
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
    // Client, dem die Figur gehört; nullopt, wenn sie keinem gehört.
    [[nodiscard]] std::optional<net::ConnectionId> client_of(u64 figure) const {
        for (const auto& [client, player] : players_) {
            if (player.figure == figure) {
                return client;
            }
        }
        return std::nullopt;
    }

    // Eine Zeile pro Spieler für den Statusbericht.
    void report() const {
        for (const auto& [client, player] : players_) {
            const world::Entity figure = game_->world().entity(player.figure);
            const auto* transform = figure ? figure.get<world::Transform>() : nullptr;
            const math::DVec3 at =
                transform != nullptr ? transform->position - spawn_ : math::DVec3{};
            const replication::EncoderClientStats stats = server_->stats(client);
            const replication::CommandQueueStats& commands = player.commands.stats();
            const replication::TerrainClientStats terrain = terrain_.stats(client);
            log::info("  Client {}: Figur {} ({:.1f} m vom Start), {} Nachrichten, {} abgelehnt, "
                      "{} begrenzt; Befehle {} verarbeitet, {} wiederholt ({} ohne Eingabe), {} "
                      "verworfen, {} übergelaufen, {} warten; {:.1f} KB/s, Schnappschuss {} B, {} "
                      "Entities, {} warten, {} ohne Basis; Terrain {} Chunks, {:.1f} KB, Beitritt "
                      "{:.1f} KB{}, {} vorgemerkt",
                      client, figure ? figure.name() : std::string_view{"fehlt"}, math::length(at),
                      player.messages, player.rejected, player.limited, commands.processed,
                      commands.repeated, commands.idle, commands.dropped, commands.overflow,
                      player.commands.size(), static_cast<f64>(stats.bytes_per_second) / 1024.0,
                      stats.last_bytes, stats.entities, stats.waiting, stats.full, terrain.chunks,
                      static_cast<f64>(terrain.bytes) / 1024.0,
                      static_cast<f64>(terrain.join_bytes) / 1024.0,
                      terrain.complete ? "" : " (Umgebung unvollständig)", terrain.pending);
        }
    }

private:
    void join(net::ConnectionId client) {
        ConnectedPlayer player;
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
        terrain_.add_client(client);
    }

    void leave(net::ConnectionId client, ConnectedPlayer& player) {
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
        terrain_.remove_client(client);
        log::info("Client {} hat das Spiel verlassen ({} Befehle)", client,
                  player.commands.stats().processed);
    }

    GameWorld* game_;
    replication::Server* server_;
    world::Entity scene_figure_;
    bool scene_figure_reserved_ = false;
    bool scene_figure_taken_ = false;
    math::DVec3 spawn_;
    input::ActionMap map_;
    u32 map_hash_;
    std::optional<usize> look_;
    std::optional<usize> dig_;
    std::optional<usize> throw_;
    u32 spawned_ = 0;
    std::map<net::ConnectionId, ConnectedPlayer> players_;
    replication::TerrainSender terrain_;
};

// Was ein Bild des Servers gekostet hat, für den Statusbericht.
export struct ServerFrameStats {
    f64 total_ms = 0.0;
    f64 max_ms = 0.0;
    u64 count = 0;
    usize snapshot_bytes = 0;
};

// Ereignisse und Einmal-Klänge der Skripte (R5.5), seit dem letzten Statusbericht.
export struct ServerNetStats {
    u64 events = 0;
    usize event_bytes = 0;
    u64 sounds = 0;
    usize sound_bytes = 0;
    // Net.Send an eine Figur, die keinem Spieler gehört.
    u64 unowned = 0;
};

// Hörweite eines Einmal-Klangs vom Server in Metern (Reichweite der Effekte).
export inline constexpr f64 sound_range = 50.0;

// Transport, Replikation und Spieler über einer Spielwelt (R5.5). Gehört dem Thread der Welt.
export class ServerSession {
public:
    // game muss die Sitzung überleben und ihre Szene schon geladen haben; Skripte sind dann schon
    // angemeldet, die Typen der Replikation kommen hier dazu (Schema wie beim Client). spawn ist
    // der Startpunkt neuer Figuren.
    [[nodiscard]] static std::expected<std::unique_ptr<ServerSession>, std::string>
    create(GameWorld& game, math::DVec3 spawn) {
        auto transport = net::Transport::create();
        if (!transport) {
            return std::unexpected(transport.error());
        }
        return std::unique_ptr<ServerSession>{
            new ServerSession{game, std::move(*transport), spawn}};
    }

    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;
    ~ServerSession() {
        if (script::ScriptSystem* scripts = game_->scripts()) {
            scripts->bind_net(nullptr);
        }
    }

    // Nimmt Clients über UDP an (0 wählt einen freien Port). Verbindungen im Speicher
    // (Transport::connect_local) gehen auch ohne.
    [[nodiscard]] std::expected<u16, std::string> listen(u16 port) {
        auto bound = transport_->listen(port);
        if (bound) {
            port_ = *bound;
        }
        return bound;
    }
    [[nodiscard]] std::optional<u16> port() const noexcept {
        return port_;
    }

    // Ein Bild des Servers, genau ein fester Schritt: Nachrichten und Spieler, Befehle,
    // before_step (z. B. die feste Spur der Figur der Szene), Steuerung, Schritt, mit 30 Hz
    // Schnappschüsse, Stand der Figuren und Terrain. Alle 300 Bilder ein Statusbericht.
    void frame(const std::function<void(u64)>& before_step = {}) {
        NGINE_PROFILE_ZONE("ServerSession::frame");
        const auto start = std::chrono::steady_clock::now();
        received_.clear();
        server_.poll(received_);
        players_.sync_players(server_.clients());
        for (const replication::Received& message : received_) {
            players_.handle(message);
        }
        players_.apply_inputs();
        if (before_step) {
            before_step(frame_);
        }
        game_->steer_players();
        (void)game_->update(1.0 / 60.0);
        players_.track_contacts();
        if (frame_ % 2 == 0) {
            players_.update_focus();
            // Felder replizierter Skripte in ihre Komponenten, damit der Schnappschuss sie sieht.
            if (script::ScriptSystem* scripts = game_->scripts()) {
                scripts->store_fields();
            }
            server_.send_snapshot();
            players_.send_owner_states();
            players_.send_terrain();
            stats_.snapshot_bytes = std::max(stats_.snapshot_bytes, server_.last_snapshot_size());
        }
        const f64 ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        stats_.total_ms += ms;
        stats_.max_ms = std::max(stats_.max_ms, ms);
        ++stats_.count;
        if (frame_ % 300 == 299) {
            report();
        }
        ++frame_;
    }

    // Statusbericht ins Log; setzt die Zeiten zurück.
    void report() {
        const physics::StepStats& stats = game_->physics().stats();
        log::info("Server: fester Schritt {}, {} Client(s), {} Entities, {} Körper ({} wach), "
                  "{} Figuren, {} repliziert; Bild im Mittel {:.2f} ms, höchstens {:.2f} ms; "
                  "Schnappschuss bis {} Bytes",
                  game_->simulation().time().tick, players_.player_count(),
                  game_->world().entity_count(), game_->physics().body_count(), stats.active_bodies,
                  stats.characters, server_.entity_count(),
                  stats_.count > 0 ? stats_.total_ms / static_cast<f64>(stats_.count) : 0.0,
                  stats_.max_ms, stats_.snapshot_bytes);
        if (net_stats_.events > 0 || net_stats_.sounds > 0 || net_stats_.unowned > 0) {
            const f64 seconds = static_cast<f64>(std::max<u64>(stats_.count, 1)) / 60.0;
            log::info(
                "  Skripte: {} Ereignisse ({:.2f} KB/s), {} Klänge ({:.2f} KB/s) an Clients, "
                "{} an Figuren ohne Spieler",
                net_stats_.events, static_cast<f64>(net_stats_.event_bytes) / 1024.0 / seconds,
                net_stats_.sounds, static_cast<f64>(net_stats_.sound_bytes) / 1024.0 / seconds,
                net_stats_.unowned);
        }
        players_.report();
        stats_ = {};
        net_stats_ = {};
    }

    // Was die Skripte seit dem letzten Statusbericht an Clients geschickt haben.
    [[nodiscard]] const ServerNetStats& net_stats() const noexcept {
        return net_stats_;
    }

    // Net.Broadcast (player 0) und Net.Send der Skripte: an Clients, die die Entity kennen, bzw.
    // an den Spieler der Figur player. Zugestellt im Takt des nächsten Schnappschusses.
    void send_event(u64 entity, u64 player, std::string_view name, f32 value) {
        const auto message = replication::encode_event({.tick = server_.tick() + 1,
                                                        .entity = entity,
                                                        .value = value,
                                                        .name = std::string{name}});
        const auto deliver = [&](net::ConnectionId client) {
            if (server_.send(client, message, net::Delivery::Reliable)) {
                ++net_stats_.events;
                net_stats_.event_bytes += message.size();
            }
        };
        if (player != 0) {
            const auto client = players_.client_of(player);
            if (!client) {
                ++net_stats_.unowned;
                return;
            }
            deliver(*client);
            return;
        }
        for (const net::ConnectionId client : server_.clients()) {
            if (server_.relevant(client, entity)) {
                deliver(client);
            }
        }
    }

    // Audio.Play der Skripte ohne Audio-Gerät: an Clients in Hörweite, ohne Ort an alle.
    void send_sound(u64 sound, std::optional<math::DVec3> position, audio::AudioBus bus, f32 volume,
                    f32 pitch) {
        const auto message = replication::encode_sound({.tick = server_.tick() + 1,
                                                        .sound = sound,
                                                        .position = position,
                                                        .bus = static_cast<u8>(bus),
                                                        .volume = volume,
                                                        .pitch = pitch});
        for (const net::ConnectionId client : server_.clients()) {
            if (position && !server_.near(client, *position, sound_range)) {
                continue;
            }
            if (server_.send(client, message, net::Delivery::Unreliable)) {
                ++net_stats_.sounds;
                net_stats_.sound_bytes += message.size();
            }
        }
    }

    [[nodiscard]] u64 frames() const noexcept {
        return frame_;
    }
    [[nodiscard]] GameServer& players() noexcept {
        return players_;
    }
    [[nodiscard]] const GameServer& players() const noexcept {
        return players_;
    }
    [[nodiscard]] replication::Server& server() noexcept {
        return server_;
    }
    [[nodiscard]] const replication::Schema& schema() const noexcept {
        return *schema_;
    }
    [[nodiscard]] net::Transport& transport() noexcept {
        return *transport_;
    }
    [[nodiscard]] GameWorld& game() noexcept {
        return *game_;
    }
    [[nodiscard]] world::Entity scene_figure() const noexcept {
        return scene_figure_;
    }

private:
    ServerSession(GameWorld& game, std::unique_ptr<net::Transport> transport, math::DVec3 spawn)
        : game_{&game}, transport_{std::move(transport)},
          schema_{(replication::register_types(game.types()),
                   std::make_unique<replication::Schema>(game.types()))},
          server_{*transport_, game.world(), *schema_}, scene_figure_{find_player(game.world())},
          players_{game, server_, scene_figure_, spawn} {
        server_.set_global(global_components(game.types()));
        net_.event = [this](u64 entity, u64 player, std::string_view name, f32 value) {
            send_event(entity, player, name, value);
        };
        net_.sound = [this](u64 sound, std::optional<math::DVec3> position, audio::AudioBus bus,
                            f32 volume,
                            f32 pitch) { send_sound(sound, position, bus, volume, pitch); };
        if (script::ScriptSystem* scripts = game.scripts()) {
            scripts->bind_net(&net_);
        }
    }

    GameWorld* game_;
    std::unique_ptr<net::Transport> transport_;
    std::unique_ptr<replication::Schema> schema_;
    replication::Server server_;
    world::Entity scene_figure_;
    GameServer players_;
    std::optional<u16> port_;
    std::vector<replication::Received> received_;
    u64 frame_ = 0;
    ServerFrameStats stats_;
    ServerNetStats net_stats_;
    script::ScriptNet net_;
};

// Wie ein Listen-Server seine Welt bekommt.
export struct ListenServerDesc {
    const asset::AssetDatabase* database = nullptr;
    const asset::AssetCache* cache = nullptr;
    // Worker der Client-Welt, die die Server-Welt mitbenutzt (ein Job-System für beide); muss den
    // Server überleben. Ohne Angabe ein eigenes.
    jobs::JobSystem* jobs = nullptr;
    // .NET für die Skripte der Server-Welt; ohne keine Skripte.
    script::ScriptRuntime* runtime = nullptr;
    // Lädt die Welt und liefert den Startpunkt neuer Figuren. Ohne Angabe der Spielplatz.
    std::function<std::expected<math::DVec3, std::string>(GameWorld&)> load;
    // UDP-Port für weitere Clients; ohne Angabe nur Verbindungen im Speicher.
    std::optional<u16> port;
};

// Server und Spielwelt im Prozess des Hosts (R5.5, NET-03): rechnet pro Bild des Hosts so viele
// Bilder des Servers, wie 1/60 s in die vergangene Zeit passen, höchstens vier. Der Host
// verbindet sich mit NetClient::connect_local(transport()).
export class ListenServer {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<ListenServer>, std::string>
    create(ListenServerDesc desc) {
        auto server = std::unique_ptr<ListenServer>{new ListenServer{desc}};
        GameWorld& game = *server->game_;
        game.animation().set_rendering(false);
        game.animation().set_parameter_sync(animation::ParameterSync::Publish);
        if (desc.runtime != nullptr) {
            game.enable_scripts(*desc.runtime, nullptr,
                                {.watch = false, .wait = true, .role = script::ScriptRole::Server});
        }
        math::DVec3 spawn{};
        if (desc.load) {
            auto loaded = desc.load(game);
            if (!loaded) {
                return std::unexpected(loaded.error());
            }
            spawn = *loaded;
        } else {
            spawn = game.load_playground().spawn;
        }
        auto session = ServerSession::create(game, spawn);
        if (!session) {
            return std::unexpected(session.error());
        }
        server->session_ = std::move(*session);
        // Das erste Bild wartet auf Terrain und Kollision um die Figuren; erst danach nimmt der
        // Server Clients an (wie der dedizierte Server).
        server->session_->frame();
        if (desc.port) {
            if (auto bound = server->session_->listen(*desc.port); !bound) {
                return std::unexpected(bound.error());
            }
        }
        log::info("Listen-Server bereit{}: {} Entities, Skripte {}",
                  server->session_->port() ? std::format(" auf Port {}", *server->session_->port())
                                           : std::string{" (nur im Speicher)"},
                  game.world().entity_count(), game.scripts_running() ? "laufen" : "aus");
        return server;
    }

    ListenServer(const ListenServer&) = delete;
    ListenServer& operator=(const ListenServer&) = delete;
    ~ListenServer() {
        // Erst die Sitzung (Transport, Replikation), dann die Welt.
        session_.reset();
        game_.reset();
    }

    // Nach dt Sekunden des Hosts: fällige Bilder des Servers. Liefert ihre Zahl.
    u32 advance(f64 dt) {
        NGINE_PROFILE_ZONE("ListenServer::advance");
        constexpr f64 step = 1.0 / 60.0;
        accumulator_ += dt;
        u32 frames = 0;
        while (accumulator_ >= step && frames < 4) {
            session_->frame();
            accumulator_ -= step;
            ++frames;
        }
        // Wer weiter zurückliegt (Laden, Haltepunkt), holt nicht alles nach.
        accumulator_ = std::min(accumulator_, step);
        return frames;
    }

    [[nodiscard]] GameWorld& game() noexcept {
        return *game_;
    }
    [[nodiscard]] ServerSession& session() noexcept {
        return *session_;
    }
    [[nodiscard]] net::Transport& transport() noexcept {
        return session_->transport();
    }

private:
    explicit ListenServer(const ListenServerDesc& desc)
        : game_{std::make_unique<GameWorld>(GameWorldDesc{.database = desc.database,
                                                          .cache = desc.cache,
                                                          .jobs = desc.jobs,
                                                          .deterministic = true})} {}

    std::unique_ptr<GameWorld> game_;
    std::unique_ptr<ServerSession> session_;
    f64 accumulator_ = 0.0;
};

} // namespace app
