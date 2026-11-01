module;

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

export module ngine.replication:session;

import ngine.core;
import ngine.net;
import ngine.reflect;
import ngine.world;
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
    // transport muss schon lauschen. world, transport und die Typen in components müssen den
    // Server überleben.
    Server(net::Transport& transport, world::World& world, ComponentList components);

    // Nimmt Clients auf, begrüßt sie und hängt ihre Nutzdaten an received an.
    void poll(std::vector<Received>& received);
    // Schickt allen Clients den aktuellen Zustand (unzuverlässig) und zählt den Tick hoch.
    void send_snapshot();
    // Nutzdaten an einen Client.
    bool send(net::ConnectionId client, std::span<const std::byte> data, net::Delivery delivery);

    [[nodiscard]] std::span<const net::ConnectionId> clients() const noexcept {
        return clients_;
    }
    [[nodiscard]] u32 tick() const noexcept {
        return tick_;
    }
    // Größe des letzten Schnappschusses in Bytes.
    [[nodiscard]] usize last_snapshot_size() const noexcept {
        return last_snapshot_size_;
    }

private:
    net::Transport* transport_;
    world::World* world_;
    std::vector<const reflect::TypeInfo*> components_;
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

class Client {
public:
    Client(net::Transport& transport, world::World& world, ComponentList components);

    // Verbindet zu "adresse:port".
    [[nodiscard]] Result<void> connect(std::string_view address);
    // Wendet Schnappschüsse an und hängt Nutzdaten des Servers an received an. Ein Fehler (falsche
    // Komponentenliste, kaputter Schnappschuss) trennt die Verbindung.
    [[nodiscard]] Result<void> poll(std::vector<Received>& received);
    bool send(std::span<const std::byte> data, net::Delivery delivery);
    void disconnect();

    [[nodiscard]] ClientState state() const noexcept {
        return state_;
    }
    [[nodiscard]] const Mirror& mirror() const noexcept {
        return mirror_;
    }
    [[nodiscard]] net::ConnectionId connection() const noexcept {
        return connection_;
    }
    [[nodiscard]] u32 snapshots_applied() const noexcept {
        return snapshots_applied_;
    }

private:
    Result<void> handle(std::span<const std::byte> message, std::vector<Received>& received);

    net::Transport* transport_;
    std::vector<const reflect::TypeInfo*> components_;
    Mirror mirror_;
    std::vector<net::Event> events_;
    net::ConnectionId connection_ = 0;
    ClientState state_ = ClientState::Disconnected;
    u32 snapshots_applied_ = 0;
};

// Hängt das Byte MessageKind::Application vor die Nutzdaten.
[[nodiscard]] std::vector<std::byte> application_message(std::span<const std::byte> data);

} // namespace ngine::replication
