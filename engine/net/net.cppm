module;

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Netzwerk-Transport (NET-01, ADR 0024): verbindungsorientierte Nachrichten über UDP mit
// Zuverlässigkeit, Fragmentierung und Verschlüsselung. Dahinter steckt GameNetworkingSockets;
// außerhalb von transport_gns.cpp kennt niemand die Bibliothek. Läuft ohne Fenster und GPU.
export module ngine.net;

import ngine.core;

export namespace ngine::net {

template <typename T>
using Result = std::expected<T, std::string>;

using ConnectionId = u32;

enum class Delivery : u8 {
    // Darf verloren gehen oder überholt werden (Zustände, die ohnehin bald neu kommen).
    Unreliable,
    // Kommt sicher und in Reihenfolge an (Ereignisse, Befehle).
    Reliable,
};

namespace event {
// Eine Verbindung steht, auf dem Server wie beim Client.
struct Connected {
    ConnectionId connection;
};
struct Disconnected {
    ConnectionId connection;
    std::string reason;
};
struct Message {
    ConnectionId connection;
    std::vector<std::byte> data;
};
} // namespace event

using Event = std::variant<event::Connected, event::Disconnected, event::Message>;

struct ConnectionStats {
    // Rundlaufzeit in Millisekunden, -1 wenn unbekannt.
    i32 ping_ms = -1;
    // Anteil der Pakete, die ankommen (0 bis 1), -1 wenn unbekannt.
    f32 quality = -1.0f;
    f32 send_rate_bytes = 0.0f;
    f32 receive_rate_bytes = 0.0f;
};

// Ein Endpunkt: Server (listen) oder Client (connect), auch beides. Mehrere Transporte im selben
// Prozess sind erlaubt (Listen-Server, Tests). Gehört einem Thread.
class Transport {
public:
    [[nodiscard]] static Result<std::unique_ptr<Transport>> create();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    ~Transport();

    // Nimmt Verbindungen auf allen Adressen an. Port 0 wählt einen freien Port; geliefert wird
    // der tatsächliche Port.
    [[nodiscard]] Result<u16> listen(u16 port);
    // Verbindet zu "adresse:port", z. B. "127.0.0.1:27020". Fertig ist die Verbindung erst mit
    // event::Connected.
    [[nodiscard]] Result<ConnectionId> connect(std::string_view address);

    // Höchstens 512 KiB pro Nachricht. false, wenn die Verbindung nicht (mehr) besteht.
    bool send(ConnectionId connection, std::span<const std::byte> data, Delivery delivery);
    void disconnect(ConnectionId connection);

    // Arbeitet Netzwerkereignisse ab und hängt sie an events an. Einmal pro Frame aufrufen.
    void poll(std::vector<Event>& events);

    [[nodiscard]] std::vector<ConnectionId> connections() const;
    [[nodiscard]] ConnectionStats stats(ConnectionId connection) const;

    // Nur für create(); Impl ist außerhalb des Moduls nicht bekannt.
    struct Impl;
    explicit Transport(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::net
