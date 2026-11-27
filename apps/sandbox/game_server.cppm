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
#include <thread>
#include <vector>

// Dedizierter Server mit Spielwelt (R5.1, NET-02): ohne Fenster und GPU rechnet er über
// app::GameWorld dieselbe Welt wie Sandbox, Editor und --headless, standardmäßig den Spielplatz mit
// Terrain, Physik, Skripten (aus dem Cache) und Automaten der Animation. Ein Bild ist genau ein
// fester Schritt zu 1/60 s; hinkt er hinterher, holt er ohne Pause auf. So hängt das Ergebnis
// nicht von der Uhr ab, und ohne Clients rechnet er bitgleich wie --headless --playground.
//
// Spieler, Befehle (R5.3), Replikation (R5.2) und Terrain (R5.4) stecken seit R5.5 in
// app::ServerSession (apps/common/game_server.cppm), die auch der Listen-Server nutzt; hier
// bleiben Takt, Laden und die feste Spur (--track).
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
import ngine.jobs;
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
import app.prediction;
import app.scripting;
import app.game_server;
import app.net_client;
import sandbox.playground;

namespace sandbox {

using namespace ngine;

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


// Ein verbundener Spieler: seine Figur und seine Befehle (R5.3).

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
        game.enable_scripts(*runtime, nullptr, {.watch = false, .wait = true, .role = script::ScriptRole::Server});
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
    auto created = app::ServerSession::create(game, spawn);
    if (!created) {
        log::error("{}", created.error());
        return 1;
    }
    app::ServerSession& session = **created;
    const world::Entity scene_figure = session.scene_figure();
    std::optional<PlaygroundTrack> track;
    if (options.track && scene_figure) {
        session.players().reserve_scene_figure();
        track.emplace(game, scene_figure);
    }
    // Gelauscht wird erst nach dem ersten Bild: Es wartet auf Terrain und Kollision um die Figuren
    // (deterministisch, im Debug-Build mehrere Sekunden), und ein Client, der in dieser Zeit
    // verbindet, bekäme keine Antwort.
    const auto listen = [&]() -> bool {
        const auto bound = session.listen(options.port.value_or(app::default_game_port));
        if (!bound) {
            log::error("{}", bound.error());
            return false;
        }
        const std::chrono::duration<f64, std::milli> load_time =
            std::chrono::steady_clock::now() - loading;
        log::info("Server bereit auf Port {}: {} Entities, {} Körper, {} replizierte Komponenten, "
                  "Skripte {}, Figur der Szene {}{}, geladen in {:.0f} ms",
                  *bound, world.entity_count(), game.physics().body_count(),
                  session.schema().size(), game.scripts_running() ? "laufen" : "aus",
                  scene_figure ? scene_figure.name() : std::string_view{"fehlt"},
                  track ? " (läuft die Spur)" : "", load_time.count());
        // Sofort sichtbar, auch wenn stdout in eine Datei geht: Skripte warten auf diese Zeile.
        log::flush();
        return true;
    };
    if (options.frames && *options.frames == 0 && !listen()) {
        return 1;
    }

    TickClock clock;
    const auto apply_track = [&](u64 frame) {
        if (track) {
            track->apply(frame);
        }
    };
    for (u64 frame = 0; !options.frames || frame < *options.frames; ++frame) {
        session.frame(apply_track);
        if (!session.port() && !listen()) {
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

// Client ohne Fenster (R5.1): schickt pro festem Schritt einen Befehl aus der festen Spur und
// meldet, wo seine Figur steht. Seit R5.2 mit der ganzen Welt als Delta, seit R5.3 mit Vorhersage
// der eigenen Figur über app::GameWorld (Physik im Modus remote), seit R5.5 mit Client-Skripten
// (dieselben Skript-Typen wie der Server, sonst passt das Schema nicht) und als Client des
// Listen-Servers (--host --headless).
class HeadlessClient {
public:
    HeadlessClient(const asset::AssetDatabase* database, const asset::AssetCache* cache,
                   script::ScriptRuntime* runtime, jobs::JobSystem* jobs = nullptr)
        : game_{{.database = database, .cache = cache, .jobs = jobs}},
          actions_{input::ActionMap::standard()} {
        game_.animation().set_rendering(false);
        game_.animation().set_parameter_sync(animation::ParameterSync::Follow);
        if (runtime != nullptr) {
            game_.enable_scripts(*runtime, nullptr,
                                 {.watch = false, .wait = true, .role = script::ScriptRole::Client});
        }
        replication::register_types(game_.types());
        schema_ = std::make_unique<replication::Schema>(game_.types());
    }
    HeadlessClient(const HeadlessClient&) = delete;
    HeadlessClient& operator=(const HeadlessClient&) = delete;

    [[nodiscard]] bool connect(const std::string& address) {
        return adopt(app::NetClient::connect(game_.world(), *schema_, address));
    }
    [[nodiscard]] bool connect_local(net::Transport& server) {
        return adopt(app::NetClient::connect_local(game_.world(), *schema_, server));
    }

    // Ein Bild; false bei einem Fehler der Verbindung.
    [[nodiscard]] bool frame(u64 frame) {
        app::NetClient& net = *net_;
        if (auto result = net.poll(); !result) {
            log::error("{}", result.error());
            return false;
        }
        if (net.disconnected()) {
            log::error("Keine Verbindung zum Server");
            return false;
        }
        prediction_->set_figure(net.figure(), net.figure_on_server(), net.client().mirror());
        for (const replication::OwnerState& owner : net.owner_states()) {
            prediction_->handle_owner(owner);
        }
        net.client().mirror().interpolate(1.0 / 60.0);
        net.deliver();
        actions_.begin_frame();
        if (prediction_->figure() != 0) {
            // Die Spur beginnt, sobald die Figur feststeht.
            for (const platform::Event& event : scripted_events(track_frame_++)) {
                actions_.handle(event);
            }
        }
        actions_.end_frame(1.0f / 60.0f);
        prediction_->look(actions_);
        (void)game_.update(1.0 / 60.0);
        prediction_->update(1.0f / 60.0f);
        world::World& world = game_.world();
        if (const auto* transform =
                prediction_->figure() != 0
                    ? world.entity(prediction_->figure()).get<world::Transform>()
                    : nullptr) {
            if (!first_) {
                first_ = transform->position;
            }
            last_ = transform->position;
        }
        if (frame % 60 == 59 && first_) {
            log::info("Client: Figur {:.2f} m vom ersten Stand, {} Entities gespiegelt, Tick {}",
                      math::length(last_ - *first_), net.client().mirror().entity_count(),
                      net.client().mirror().last_tick());
        }
        if (frame % 180 == 179) {
            log::info("{}", net.report());
            log::info("{}", app::describe_prediction(prediction_->report()));
        }
        return true;
    }

    // Trennt und prüft: Fehlschlag, wenn keine Figur kam oder sie sich nicht bewegt hat.
    [[nodiscard]] int finish(const std::function<void()>& pump = {}) {
        app::NetClient& net = *net_;
        log::info("Client: {} Schnappschüsse angewendet", net.client().snapshots_applied());
        log::info("{}", net.report());
        log::info("{}", app::describe_prediction(prediction_->report()));
        net.disconnect();
        // Kurz weiterpollen, damit die Trennung beim Server ankommt.
        for (int i = 0; i < 10; ++i) {
            (void)net.poll();
            if (pump) {
                pump();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        if (net.figure_on_server() == 0 || !first_) {
            log::error("Keine Figur vom Server bekommen");
            return 1;
        }
        const f64 moved = math::length(last_ - *first_);
        if (moved < 1.0) {
            log::error("Die Figur hat sich nicht bewegt ({:.2f} m)", moved);
            return 1;
        }
        log::info("Client: Figur ist {:.1f} m gelaufen (vorhergesagt)", moved);
        return 0;
    }

    [[nodiscard]] app::NetClient& net() noexcept {
        return *net_;
    }
    [[nodiscard]] app::GameWorld& game() noexcept {
        return game_;
    }

private:
    bool adopt(std::expected<std::unique_ptr<app::NetClient>, std::string> connected) {
        if (!connected) {
            log::error("{}", connected.error());
            return false;
        }
        net_ = std::move(*connected);
        net_->set_scripts(game_.scripts());
        app::NetClient* net = net_.get();
        prediction_ = std::make_unique<app::PlayerPrediction>(
            game_, actions_, [net](std::span<const std::byte> data) { net->send_game(data); });
        prediction_->set_terrain(&net_->terrain());
        return true;
    }

    app::GameWorld game_;
    std::unique_ptr<replication::Schema> schema_;
    input::ActionState actions_;
    // Vor der Vorhersage zerstört, die an der Welt hängt.
    std::unique_ptr<app::PlayerPrediction> prediction_;
    std::unique_ptr<app::NetClient> net_;
    std::optional<math::DVec3> first_;
    math::DVec3 last_{};
    u64 track_frame_ = 0;
};

export int run_game_client(const std::string& address, u64 frames,
                           const asset::AssetDatabase* database, const asset::AssetCache* cache,
                           script::ScriptRuntime* runtime) {
    HeadlessClient client{database, cache, runtime};
    if (!client.connect(address)) {
        return 1;
    }
    TickClock clock;
    for (u64 frame = 0; frame < frames; ++frame) {
        if (!client.frame(frame)) {
            return 1;
        }
        clock.wait();
    }
    return client.finish();
}

export struct ListenServerOptions {
    u64 frames = 600;
    std::optional<u16> port;
    const asset::AssetDatabase* database = nullptr;
    const asset::AssetCache* cache = nullptr;
    script::ScriptRuntime* runtime = nullptr;
};

// Listen-Server ohne Fenster (--host --headless, R5.5): Server und Client in einem Prozess mit
// einem Job-System und zwei Skript-Welten; der Client verbindet sich im Speicher und läuft die
// feste Spur. Weitere Clients kommen über UDP (Port, Standard 27020). Misst die Zeit der
// Server-Bilder pro Bild des Hosts.
export int run_listen_server(const ListenServerOptions& options) {
    // Der Client zuerst: Seine Welt besitzt das Job-System, das der Server mitbenutzt.
    HeadlessClient client{options.database, options.cache, options.runtime};
    auto created = app::ListenServer::create({.database = options.database,
                                              .cache = options.cache,
                                              .jobs = &client.game().jobs(),
                                              .runtime = options.runtime,
                                              .load = {},
                                              .port = options.port.value_or(app::default_game_port)});
    if (!created) {
        log::error("{}", created.error());
        return 1;
    }
    app::ListenServer& host = **created;
    // Skripte laut stdout auf diese Zeile wartend (CI).
    log::flush();
    if (!client.connect_local(host.transport())) {
        return 1;
    }
    TickClock clock;
    f64 server_ms = 0.0;
    f64 server_max_ms = 0.0;
    for (u64 frame = 0; frame < options.frames; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        (void)host.advance(1.0 / 60.0);
        const f64 ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        server_ms += ms;
        server_max_ms = std::max(server_max_ms, ms);
        if (!client.frame(frame)) {
            return 1;
        }
        clock.wait();
    }
    log::info("Listen-Server: Server-Bilder im Mittel {:.2f} ms pro Bild des Hosts, höchstens "
              "{:.2f} ms; {} Client(s), {} Entities auf dem Server, {} gespiegelt",
              server_ms / static_cast<f64>(std::max<u64>(options.frames, 1)), server_max_ms,
              host.session().players().player_count(), host.game().world().entity_count(),
              client.net().client().mirror().entity_count());
    const int result = client.finish([&] { (void)host.advance(1.0 / 60.0); });
    log::info("Prüfsumme der Lagen auf dem Server: {:016x}",
              physics::checksum(host.game().world()));
    return result;
}

namespace {

// Ein Bot: eigene Welt, eigene Verbindung, läuft die feste Spur versetzt und im Kreis.
// Die Skript-Typen kommen vor Replicated in die Registry, wie beim Server (R5.5).
struct Bot {
    explicit Bot(script::ScriptRuntime* runtime, const asset::AssetDatabase* database)
        : types{app::make_game_types()},
          scripts{app::make_client_scripts(runtime, types, database)},
          world{(replication::register_types(types), std::make_unique<world::World>(types))},
          schema{std::make_unique<replication::Schema>(types)} {}

    reflect::TypeRegistry types;
    std::unique_ptr<script::ScriptSystem> scripts;
    std::unique_ptr<world::World> world;
    std::unique_ptr<replication::Schema> schema;
    std::unique_ptr<app::NetClient> net;
    input::ActionState actions{input::ActionMap::standard()};
    u64 offset = 0;
    u64 track_frame = 0;
    // Bytes der Schnappschüsse pro Sekunde, einmal pro Sekunde gemessen.
    std::vector<usize> rates;
};

} // namespace

// count Clients ohne Fenster in einem Prozess (R5.2): messen Bandbreite pro Client. Fehlschlag,
// wenn einer keine Figur bekommt oder getrennt wird.
export int run_bots(const std::string& address, u32 count, u64 frames,
                    const asset::AssetDatabase* database, script::ScriptRuntime* runtime) {
    std::vector<std::unique_ptr<Bot>> bots;
    for (u32 i = 0; i < count; ++i) {
        auto bot = std::make_unique<Bot>(runtime, database);
        auto connected = app::NetClient::connect(*bot->world, *bot->schema, address);
        if (!connected) {
            log::error("Bot {}: {}", i + 1, connected.error());
            return 1;
        }
        bot->net = std::move(*connected);
        // Bots haben kein Terrain; die Deltas zählen nur für die Bandbreite (R5.4).
        bot->net->set_apply_terrain(false);
        bot->offset = static_cast<u64>(i) * 97;
        bots.push_back(std::move(bot));
    }
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
            bot.net->send_command(bot.actions);
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
