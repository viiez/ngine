module;

#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module ngine.replication:session;

import ngine.core;
import ngine.math;
import ngine.net;
import ngine.reflect;
import ngine.world;
import :schema;
import :snapshot;

// Server und Client über einem net::Transport. Beide gehören dem Thread, der die Welt besitzt,
// und werden einmal pro Frame abgefragt.
export namespace ngine::replication {

// Nutzdaten der Anwendung, die über die Replikation mitlaufen.
struct Received {
    net::ConnectionId connection = 0;
    std::vector<std::byte> data;
};

class Server {
public:
    // transport muss schon lauschen. world, transport und schema müssen den Server überleben.
    Server(net::Transport& transport, world::World& world, const Schema& schema,
           EncoderSettings settings = {});

    // Entities mit diesen Komponenten sind für alle Clients relevant (Terrain, Sonne).
    void set_global(std::span<const reflect::TypeInfo* const> types);
    // Mittelpunkt der Relevanz eines Clients (seine Figur).
    void set_focus(net::ConnectionId client, std::optional<math::DVec3> focus);

    // Nimmt Clients auf, begrüßt sie, nimmt Bestätigungen an und hängt Nutzdaten an received an.
    void poll(std::vector<Received>& received);
    // Zählt den Tick hoch, liest die Welt und schickt jedem Client sein Delta (unzuverlässig).
    void send_snapshot();
    // Nutzdaten an einen Client.
    bool send(net::ConnectionId client, std::span<const std::byte> data, net::Delivery delivery);

    [[nodiscard]] std::span<const net::ConnectionId> clients() const noexcept {
        return clients_;
    }
    [[nodiscard]] u32 tick() const noexcept {
        return tick_;
    }
    // Größter Schnappschuss des letzten Ticks über alle Clients, in Bytes.
    [[nodiscard]] usize last_snapshot_size() const noexcept {
        return last_snapshot_size_;
    }
    [[nodiscard]] EncoderClientStats stats(net::ConnectionId client) const {
        return encoder_.stats(client);
    }
    [[nodiscard]] usize entity_count() const noexcept {
        return encoder_.entity_count();
    }

private:
    net::Transport* transport_;
    const Schema* schema_;
    SnapshotEncoder encoder_;
    std::vector<net::ConnectionId> clients_;
    std::vector<net::Event> events_;
    u32 tick_ = 0;
    usize last_snapshot_size_ = 0;
};

enum class ClientState : u8 {
    Connecting,
    // Willkommensnachricht passt, Schnappschüsse werden angewendet.
    Ready,
    Disconnected,
};

// Was der Client über die Verbindung weiß (Fenster „Netzwerk“, Log).
struct ClientStats {
    // Rundlaufzeit und Anteil verlorener Pakete laut Transport (-1 unbekannt).
    i32 ping_ms = -1;
    f32 loss = -1.0f;
    f32 send_bytes_per_second = 0.0f;
    f32 receive_bytes_per_second = 0.0f;
    // Angewendete Schnappschüsse in der letzten Sekunde und ihre Bytes.
    u32 snapshots_per_second = 0;
    usize snapshot_bytes_per_second = 0;
    usize last_snapshot_bytes = 0;
    usize largest_snapshot_bytes = 0;
    MirrorStats mirror;
};

class Client {
public:
    Client(net::Transport& transport, world::World& world, const Schema& schema);

    // Verbindet zu "adresse:port".
    [[nodiscard]] Result<void> connect(std::string_view address);
    // Verbindung im Speicher zum Transport eines Servers im selben Prozess (R5.5).
    [[nodiscard]] Result<void> connect_local(net::Transport& server);
    // Wendet Schnappschüsse an, bestätigt sie und hängt Nutzdaten des Servers an received an. Ein
    // Fehler (anderes Schema, kaputter Schnappschuss) trennt die Verbindung.
    [[nodiscard]] Result<void> poll(std::vector<Received>& received);
    bool send(std::span<const std::byte> data, net::Delivery delivery);
    void disconnect();

    [[nodiscard]] ClientState state() const noexcept {
        return state_;
    }
    [[nodiscard]] const Mirror& mirror() const noexcept {
        return mirror_;
    }
    [[nodiscard]] Mirror& mirror() noexcept {
        return mirror_;
    }
    [[nodiscard]] net::ConnectionId connection() const noexcept {
        return connection_;
    }
    [[nodiscard]] u32 snapshots_applied() const noexcept {
        return snapshots_applied_;
    }
    [[nodiscard]] ClientStats stats() const;

private:
    Result<void> handle(std::span<const std::byte> message, std::vector<Received>& received);

    using Clock = std::chrono::steady_clock;
    struct Arrival {
        Clock::time_point time;
        usize bytes = 0;
    };

    net::Transport* transport_;
    const Schema* schema_;
    Mirror mirror_;
    std::vector<net::Event> events_;
    net::ConnectionId connection_ = 0;
    ClientState state_ = ClientState::Disconnected;
    u32 snapshots_applied_ = 0;
    std::deque<Arrival> arrivals_;
    usize last_snapshot_bytes_ = 0;
    usize largest_snapshot_bytes_ = 0;
};

// Hängt das Byte MessageKind::Application vor die Nutzdaten.
[[nodiscard]] std::vector<std::byte> application_message(std::span<const std::byte> data);

} // namespace ngine::replication
