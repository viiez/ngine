module;

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Verbindung eines Clients zum Server mit Spielwelt (R5.2): Transport, Replikation in die eigene
// Welt, welche Figur ihm gehört, Befehle zum Server und der Stand der eigenen Figur zurück (R5.3),
// seit R5.4 die Deltas des Terrains (replication::TerrainReceiver).
// Die Sandbox mit Fenster (--client), der Client ohne Fenster (--client --headless), die Bots
// (--bots) und seit R5.5 der Client des Listen-Servers (Sandbox --host, Editor) teilen das; bis
// R5.4 stand es in der Sandbox.
export module app.net_client;

import ngine.core;
import ngine.input;
import ngine.net;
import ngine.replication;
import ngine.world;

namespace app {

using namespace ngine;

export constexpr u16 default_game_port = 27020;

// "host" wird zu "host:27020", "host:port" bleibt.
export std::string game_address(std::string_view address) {
    return address.find(':') == std::string_view::npos
               ? std::format("{}:{}", address, default_game_port)
               : std::string{address};
}

export class NetClient {
public:
    // Verbindet zu address (mit oder ohne Port). world und schema müssen den Client überleben.
    [[nodiscard]] static std::expected<std::unique_ptr<NetClient>, std::string>
    connect(world::World& world, const replication::Schema& schema, std::string_view address) {
        auto transport = net::Transport::create();
        if (!transport) {
            return std::unexpected(transport.error());
        }
        auto client =
            std::unique_ptr<NetClient>{new NetClient{std::move(*transport), world, schema}};
        const std::string target = game_address(address);
        if (auto result = client->client_.connect(target); !result) {
            return std::unexpected(result.error());
        }
        client->address_ = target;
        log::info("Verbinde zu {}", target);
        return client;
    }

    // Verbindung im Speicher zum Transport eines Servers im selben Prozess (R5.5, Listen-Server).
    // world, schema und server müssen den Client überleben.
    [[nodiscard]] static std::expected<std::unique_ptr<NetClient>, std::string>
    connect_local(world::World& world, const replication::Schema& schema,
                  net::Transport& server) {
        auto transport = net::Transport::create();
        if (!transport) {
            return std::unexpected(transport.error());
        }
        auto client =
            std::unique_ptr<NetClient>{new NetClient{std::move(*transport), world, schema}};
        if (auto result = client->client_.connect_local(server); !result) {
            return std::unexpected(result.error());
        }
        client->address_ = "Speicher";
        return client;
    }

    // Einmal pro Bild: Schnappschüsse anwenden und bestätigen, Nachrichten des Spiels lesen.
    [[nodiscard]] std::expected<void, std::string> poll() {
        received_.clear();
        if (auto result = client_.poll(received_); !result) {
            return result;
        }
        owner_states_.clear();
        for (const replication::Received& message : received_) {
            const auto kind = replication::game_message(message.data);
            if (kind == replication::GameMessage::Possess) {
                if (auto possessed = replication::decode_possess(message.data)) {
                    figure_ = *possessed;
                    log::info("Client: steuere Figur {} des Servers", figure_);
                }
            } else if (kind == replication::GameMessage::Owner) {
                if (auto owner = replication::decode_owner(message.data)) {
                    owner_states_.push_back(*owner);
                }
            } else if (kind == replication::GameMessage::Terrain) {
                if (!apply_terrain_) {
                    terrain_bytes_ += message.data.size();
                } else if (auto result = terrain_.receive(message.data);
                           !result && terrain_errors_++ == 0) {
                    log::warn("Client: {}", result.error());
                }
            }
        }
        if (apply_terrain_) {
            // Deltas für Terrains, die der Spiegel kennt und die gebaut sind (R5.4).
            (void)terrain_.apply(*world_, client_.mirror());
            for (const replication::OwnerState& owner : owner_states_) {
                terrain_.expire(*world_, owner.command);
            }
        }
        return {};
    }

    // Eine Nachricht des Spiels an den Server (Befehle, R5.3), unzuverlässig.
    void send_game(std::span<const std::byte> data) {
        if (ready()) {
            (void)client_.send(data, net::Delivery::Unreliable);
        }
    }

    // Befehl aus den Aktionen eines festen Schritts für Clients ohne Vorhersage (Bots): Der Blick
    // folgt der Mausbewegung, die neuesten drei Befehle gehen an den Server.
    void send_command(const input::ActionState& actions) {
        if (!ready()) {
            return;
        }
        const input::ActionMap& map = actions.map();
        replication::Command command{.number = next_command_++,
                                     .yaw = 0.0f,
                                     .pitch = 0.0f,
                                     .actions = std::vector<input::ActionValue>(
                                         actions.values().begin(), actions.values().end())};
        if (const auto look = map.find("look"); look && *look < command.actions.size()) {
            yaw_ -= command.actions[*look].x * 0.0025f;
            pitch_ = std::clamp(pitch_ - command.actions[*look].y * 0.0025f, -1.55f, 1.55f);
            command.actions[*look] = {};
        }
        command.yaw = yaw_;
        command.pitch = pitch_;
        recent_.push_back(std::move(command));
        while (recent_.size() > replication::commands_per_message) {
            recent_.erase(recent_.begin());
        }
        send_game(replication::encode_commands(map.hash(), recent_));
    }

    // Deltas des Terrains vom Server und vorhergesagte Pinsel (R5.4).
    [[nodiscard]] replication::TerrainReceiver& terrain() noexcept {
        return terrain_;
    }
    [[nodiscard]] const replication::TerrainReceiver& terrain() const noexcept {
        return terrain_;
    }
    // Ohne Terrain (Bots): Nachrichten nur zählen, nicht anwenden.
    void set_apply_terrain(bool apply) noexcept {
        apply_terrain_ = apply;
    }
    [[nodiscard]] usize terrain_bytes() const noexcept {
        return apply_terrain_ ? static_cast<usize>(terrain_.stats().bytes) : terrain_bytes_;
    }

    // Stände der eigenen Figur, die in diesem Bild angekommen sind.
    [[nodiscard]] std::span<const replication::OwnerState> owner_states() const noexcept {
        return owner_states_;
    }

    void disconnect() {
        client_.disconnect();
    }

    // Verbunden, begrüßt und mit Figur.
    [[nodiscard]] bool ready() const noexcept {
        return client_.state() == replication::ClientState::Ready && figure_ != 0;
    }
    [[nodiscard]] bool disconnected() const noexcept {
        return client_.state() == replication::ClientState::Disconnected;
    }
    // Kennung der eigenen Figur auf dem Server bzw. in der eigenen Welt (0, solange unbekannt).
    [[nodiscard]] u64 figure_on_server() const noexcept {
        return figure_;
    }
    [[nodiscard]] u64 figure() const noexcept {
        return figure_ != 0 ? client_.mirror().local_id(figure_) : 0;
    }
    [[nodiscard]] replication::Client& client() noexcept {
        return client_;
    }
    [[nodiscard]] const replication::Client& client() const noexcept {
        return client_;
    }
    [[nodiscard]] const std::string& address() const noexcept {
        return address_;
    }

    // Eine Zeile für das Log.
    [[nodiscard]] std::string report() const {
        const replication::ClientStats stats = client_.stats();
        return std::format(
            "Netz: Ping {} ms, Verlust {}, {} Schnappschüsse/s mit {:.1f} KB/s (zuletzt {} B, "
            "höchstens {} B), empfangen {:.1f} KB/s, gesendet {:.1f} KB/s; {} Entities, Puffer "
            "{:.1f} Ticks, {} interpoliert, {} ohne Basis, {} zu spät; Terrain {:.1f} KB{}",
            stats.ping_ms,
            stats.loss >= 0.0f ? std::format("{:.1f} %", stats.loss * 100.0f) : std::string{"?"},
            stats.snapshots_per_second, static_cast<f64>(stats.snapshot_bytes_per_second) / 1024.0,
            stats.last_snapshot_bytes, stats.largest_snapshot_bytes,
            stats.receive_bytes_per_second / 1024.0f, stats.send_bytes_per_second / 1024.0f,
            client_.mirror().entity_count(), stats.mirror.buffer_ticks, stats.mirror.interpolated,
            stats.mirror.missing_base, stats.mirror.stale,
            static_cast<f64>(terrain_bytes()) / 1024.0,
            apply_terrain_ ? std::format(" in {} Chunks, Umgebung {}, Pinsel {} offen, {} "
                                         "bestätigt, {} abweichend, {} verfallen",
                                         terrain_.stats().chunks,
                                         terrain_.complete() ? "vollständig" : "fehlt",
                                         terrain_.stats().predicted, terrain_.stats().confirmed,
                                         terrain_.stats().mismatched, terrain_.stats().expired)
                           : std::string{});
    }

private:
    NetClient(std::unique_ptr<net::Transport> transport, world::World& world,
              const replication::Schema& schema)
        : transport_{std::move(transport)}, client_{*transport_, world, schema}, world_{&world} {}

    std::unique_ptr<net::Transport> transport_;
    replication::Client client_;
    std::vector<replication::Received> received_;
    std::string address_;
    u64 figure_ = 0;
    std::vector<replication::OwnerState> owner_states_;
    world::World* world_;
    replication::TerrainReceiver terrain_;
    bool apply_terrain_ = true;
    usize terrain_bytes_ = 0;
    u32 terrain_errors_ = 0;
    // Nur für send_command().
    std::vector<replication::Command> recent_;
    u32 next_command_ = 1;
    f32 yaw_ = 0.0f;
    f32 pitch_ = 0.0f;
};

} // namespace app
