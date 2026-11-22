module;

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
// Welt, welche Figur ihm gehört, Eingaben und Blick zum Server. Die Sandbox mit Fenster (--client),
// der Client ohne Fenster (--client --headless) und die Bots (--bots) teilen das.
export module sandbox.net_client;

import ngine.core;
import ngine.input;
import ngine.net;
import ngine.replication;
import ngine.world;

namespace sandbox {

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

    // Einmal pro Bild: Schnappschüsse anwenden und bestätigen, Nachrichten des Spiels lesen.
    [[nodiscard]] std::expected<void, std::string> poll() {
        received_.clear();
        if (auto result = client_.poll(received_); !result) {
            return result;
        }
        for (const replication::Received& message : received_) {
            if (auto possessed = replication::decode_possess(message.data)) {
                figure_ = *possessed;
                log::info("Client: steuere Figur {} des Servers", figure_);
            }
        }
        return {};
    }

    // Aktionen dieses Bildes und, wenn der Client die Kamera selbst dreht, der Blick.
    void send_input(u32 map_hash, std::span<const input::ActionValue> values,
                    std::optional<replication::ViewAngles> view = std::nullopt) {
        if (!ready()) {
            return;
        }
        (void)client_.send(replication::encode_actions(map_hash, values),
                           net::Delivery::Unreliable);
        if (view) {
            (void)client_.send(replication::encode_view(view->yaw, view->pitch),
                               net::Delivery::Unreliable);
        }
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
            "{:.1f} Ticks, {} interpoliert, {} ohne Basis, {} zu spät",
            stats.ping_ms,
            stats.loss >= 0.0f ? std::format("{:.1f} %", stats.loss * 100.0f) : std::string{"?"},
            stats.snapshots_per_second, static_cast<f64>(stats.snapshot_bytes_per_second) / 1024.0,
            stats.last_snapshot_bytes, stats.largest_snapshot_bytes,
            stats.receive_bytes_per_second / 1024.0f, stats.send_bytes_per_second / 1024.0f,
            client_.mirror().entity_count(), stats.mirror.buffer_ticks, stats.mirror.interpolated,
            stats.mirror.missing_base, stats.mirror.stale);
    }

private:
    NetClient(std::unique_ptr<net::Transport> transport, world::World& world,
              const replication::Schema& schema)
        : transport_{std::move(transport)}, client_{*transport_, world, schema} {}

    std::unique_ptr<net::Transport> transport_;
    replication::Client client_;
    std::vector<replication::Received> received_;
    std::string address_;
    u64 figure_ = 0;
};

} // namespace sandbox
