module;

#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>

// Spielen als Listen-Server im Editor (R5.5, NET-03, ADR 0055). Wie beim Spielen sichert das
// Dokument die Welt (ADR 0032). Aus der Sicherung entsteht die Welt des Servers in einem eigenen
// app::ListenServer (mit den Deltas der Terrains); die Welt des Dokuments wird leer und zum Spiegel
// des Clients: Viewport, Audio, Vorhersage der eigenen Figur und Client-Skripte arbeiten wie in der
// Sandbox mit --host. Ein zweites Dokument über der Welt des Servers (nur lesbar) zeigt Szenenbaum
// und Inspector dort, wo Skriptfelder rechnen. Beim Stoppen endet der Server, und end_play() stellt
// die Welt des Dokuments bitgenau wieder her.
export module editor.listen_play;

import ngine.core;
import ngine.asset;
import ngine.edit;
import ngine.editor_ui;
import ngine.input;
import ngine.math;
import ngine.net;
import ngine.animation;
import ngine.replication;
import ngine.script;
import ngine.world;
import app.game_server;
import app.model_scene;
import app.net_client;
import app.player;
import app.prediction;

namespace editor {

using namespace ngine;

export struct ListenPlayDesc {
    const asset::AssetDatabase* database = nullptr;
    const asset::AssetCache* cache = nullptr;
    script::ScriptRuntime* runtime = nullptr;
    u16 port = app::default_game_port;
};

export class ListenPlay {
public:
    // Nach document.begin_play(): Server aus der Sicherung, Welt des Dokuments leeren, verbinden.
    // scene, document und actions müssen das Spielen überleben.
    [[nodiscard]] static std::expected<std::unique_ptr<ListenPlay>, std::string>
    start(app::ModelScene& scene, edit::SceneDocument& document, input::ActionState& actions,
          const ListenPlayDesc& desc) {
        auto play = std::unique_ptr<ListenPlay>{new ListenPlay{scene, actions}};
        // Client-Skripte und Typen der Replikation in der Registry der Szene, wie beim Server.
        if (script::ScriptSystem* scripts = scene.scripts()) {
            scripts->set_role(script::ScriptRole::Client);
        }
        replication::register_types(scene.game().types());
        play->schema_ = std::make_unique<replication::Schema>(scene.game().types());

        auto hosted = app::ListenServer::create(
            {.database = desc.database,
             .cache = desc.cache,
             .jobs = &scene.game().jobs(),
             .runtime = desc.runtime,
             .load = [&document](app::GameWorld& game) -> std::expected<math::DVec3, std::string> {
                 auto copied = document.copy_play_backup(game.world());
                 if (!copied) {
                     return std::unexpected(copied.error());
                 }
                 const world::Entity player = app::find_player(game.world());
                 const auto* transform = player ? player.get<world::Transform>() : nullptr;
                 return transform != nullptr ? transform->position : math::DVec3{};
             },
             .port = desc.port});
        if (!hosted) {
            play->restore_scene();
            return std::unexpected(hosted.error());
        }
        play->server_ = std::move(*hosted);

        if (auto cleared = document.clear_while_playing(); !cleared) {
            play->restore_scene();
            return std::unexpected(cleared.error());
        }
        scene.animation().set_parameter_sync(animation::ParameterSync::Follow);
        auto connected = app::NetClient::connect_local(scene.world(), *play->schema_,
                                                       play->server_->transport());
        if (!connected) {
            play->restore_scene();
            return std::unexpected(connected.error());
        }
        play->net_ = std::move(*connected);
        play->net_->set_scripts(scene.scripts());
        play->net_->set_audio(scene.audio());
        app::NetClient* net = play->net_.get();
        play->prediction_ = std::make_unique<app::PlayerPrediction>(
            scene.game(), actions,
            [net](std::span<const std::byte> data) { net->send_game(data); });
        play->prediction_->set_terrain(&play->net_->terrain());
        play->server_document_ =
            std::make_unique<edit::SceneDocument>(play->server_->game().world(), nullptr);
        play->server_document_->set_locked(true);
        return play;
    }

    ListenPlay(const ListenPlay&) = delete;
    ListenPlay& operator=(const ListenPlay&) = delete;
    ~ListenPlay() {
        // Erst der Client (Vorhersage, Verbindung), dann der Server, dann die Szene wie vorher.
        if (net_) {
            net_->disconnect();
        }
        prediction_.reset();
        net_.reset();
        server_document_.reset();
        server_.reset();
        restore_scene();
    }

    // Ein Bild: Server-Bilder, Schnappschüsse, Vorhersage, Szene. paused hält den Server an;
    // false, wenn die Verbindung verloren ist.
    [[nodiscard]] bool frame(f32 dt, bool paused) {
        if (!paused) {
            (void)server_->advance(dt);
        }
        if (auto result = net_->poll(); !result) {
            log::error("Spielen als Listen-Server: {}", result.error());
            return false;
        }
        if (net_->disconnected()) {
            log::error("Spielen als Listen-Server: Verbindung verloren");
            return false;
        }
        prediction_->set_figure(net_->figure(), net_->figure_on_server(), net_->client().mirror());
        for (const replication::OwnerState& owner : net_->owner_states()) {
            prediction_->handle_owner(owner);
        }
        net_->client().mirror().interpolate(dt);
        net_->deliver();
        prediction_->look(*actions_);
        scene_->update_predicted(dt, prediction_->figure());
        prediction_->update(dt);
        return true;
    }

    // Dokument über der Welt des Servers (nur lesbar).
    [[nodiscard]] edit::SceneDocument& server_document() noexcept {
        return *server_document_;
    }
    [[nodiscard]] app::ListenServer& server() noexcept {
        return *server_;
    }
    [[nodiscard]] app::NetClient& net() noexcept {
        return *net_;
    }
    // Eigene Figur in der Welt des Dokuments (0, solange unbekannt).
    [[nodiscard]] u64 figure() const noexcept {
        return prediction_->figure();
    }

    // Fenster „Netzwerk“: Verbindung des Hosts und Spieler des Servers.
    void draw_window(bool* open) {
        namespace ui = editor_ui;
        if (!ui::begin_window("Netzwerk", open)) {
            ui::end_window();
            return;
        }
        app::ServerSession& session = server_->session();
        ui::text(std::format("Listen-Server{}, Tick {}",
                             session.port() ? std::format(" auf Port {}", *session.port())
                                            : std::string{" (nur im Speicher)"},
                             session.server().tick()));
        ui::text(std::format("Server: {} Entities, {} repliziert",
                             server_->game().world().entity_count(),
                             session.server().entity_count()));
        ui::text(std::format("Weitere Clients: ngine_sandbox --client <adresse>{}",
                             session.port() ? std::format(":{}", *session.port()) : std::string{}));
        ui::separator();
        for (const net::ConnectionId client : session.server().clients()) {
            const replication::EncoderClientStats stats = session.server().stats(client);
            ui::text(std::format("Client {}: Figur {}, {:.1f} KB/s, {} Entities", client,
                                 session.players().figure_of(client),
                                 static_cast<double>(stats.bytes_per_second) / 1024.0,
                                 stats.entities));
        }
        ui::separator();
        const app::NetClient::EventStats& events = net_->event_stats();
        const app::PredictionReport prediction = prediction_->report();
        ui::text(std::format("Host: {} Entities gespiegelt, Puffer {:.1f} Ticks",
                             net_->client().mirror().entity_count(),
                             net_->client().stats().mirror.buffer_ticks));
        ui::text(std::format("Korrekturen {} ({} pro Minute)", prediction.predictor.corrections,
                             prediction.corrections_per_minute));
        ui::text(std::format("Ereignisse {} ({} zugestellt, {} verworfen), Klänge {}",
                             events.received, events.delivered, events.dropped, events.sounds));
        ui::end_window();
    }

private:
    ListenPlay(app::ModelScene& scene, input::ActionState& actions)
        : scene_{&scene}, actions_{&actions} {}

    // Szene wie beim Spielen ohne Netz: Skripte lokal, Automaten ohne Abgleich, Physik lokal.
    void restore_scene() {
        if (restored_) {
            return;
        }
        restored_ = true;
        if (script::ScriptSystem* scripts = scene_->scripts(); scripts != nullptr) {
            scripts->set_active(false);
            scripts->set_role(script::ScriptRole::Local);
        }
        scene_->animation().set_parameter_sync(animation::ParameterSync::None);
        scene_->game().physics().set_remote(false);
    }

    app::ModelScene* scene_;
    input::ActionState* actions_;
    std::unique_ptr<replication::Schema> schema_;
    std::unique_ptr<app::ListenServer> server_;
    std::unique_ptr<app::NetClient> net_;
    std::unique_ptr<app::PlayerPrediction> prediction_;
    std::unique_ptr<edit::SceneDocument> server_document_;
    bool restored_ = false;
};

} // namespace editor
