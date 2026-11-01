module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// Durchstich D3: dedizierter Server und Client über GameNetworkingSockets. Der Server bewegt
// Kugeln auf Kreisbahnen und je Client eine Spielfigur, die der Client über Eingaben steuert; die
// Positionen kommen als Schnappschüsse beim Client an.
export module sandbox.net_demo;

import ngine.core;
import ngine.net;
import ngine.profile;
import ngine.reflect;
import ngine.replication;
import ngine.world;

namespace sandbox {

using namespace ngine;

namespace {

struct Position {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
};

// Nur auf dem Server: Bahn einer Kugel.
struct Orbit {
    f32 radius = 1.0f;
    f32 speed = 1.0f;
    f32 angle = 0.0f;
};

// Eingabe eines Clients: Richtung in Hundertsteln (-100 bis 100).
struct Input {
    i8 x = 0;
    i8 z = 0;
};

constexpr u16 default_port = 27020;
constexpr f32 frame_time = 1.0f / 60.0f;
// Schnappschüsse mit 30 Hz, jeden zweiten Frame.
constexpr u64 snapshot_interval = 2;
constexpr u32 orbit_count = 32;
constexpr f32 player_speed = 4.0f;

struct Types {
    reflect::TypeRegistry registry;
    std::array<const reflect::TypeInfo*, 2> replicated{};

    Types() {
        world::register_types(registry);
        replication::register_types(registry);
        registry.add<Position>("Position")
            .field("x", &Position::x)
            .field("y", &Position::y)
            .field("z", &Position::z);
        registry.add<Orbit>("Orbit")
            .field("radius", &Orbit::radius)
            .field("speed", &Orbit::speed)
            .field("angle", &Orbit::angle);
        replicated = {&registry.get<world::Name>(), &registry.get<Position>()};
    }
};

// Wartet bis zum nächsten Frame, damit Server und Client mit 60 Hz laufen.
class FrameClock {
public:
    void wait() {
        next_ += std::chrono::microseconds{16'667};
        std::this_thread::sleep_until(next_);
        profile::frame_mark();
    }

private:
    std::chrono::steady_clock::time_point next_ = std::chrono::steady_clock::now();
};

std::array<std::byte, 2> encode_input(Input input) {
    return {static_cast<std::byte>(input.x), static_cast<std::byte>(input.z)};
}

// Eine Eingabe, die sich langsam dreht: der Spieler läuft im Kreis.
Input scripted_input(u64 frame) {
    const f32 angle = static_cast<f32>(frame) * 0.02f;
    return {static_cast<i8>(std::cos(angle) * 100.0f), static_cast<i8>(std::sin(angle) * 100.0f)};
}

// Spielwelt des Servers.
class ServerGame {
public:
    explicit ServerGame(world::World& world) : world_{&world} {
        for (u32 i = 0; i < orbit_count; ++i) {
            world.create(std::format("Kugel {}", i))
                .set(replication::Replicated{})
                .set(Position{})
                .set(Orbit{.radius = 3.0f + static_cast<f32>(i % 8),
                           .speed = 0.5f + static_cast<f32>(i % 5) * 0.25f,
                           .angle = static_cast<f32>(i) * 2.0f * std::numbers::pi_v<f32> /
                                    static_cast<f32>(orbit_count)});
        }
    }

    // Legt Spielfiguren für neue Clients an und räumt die von getrennten weg.
    void sync_players(std::span<const net::ConnectionId> clients) {
        for (const net::ConnectionId client : clients) {
            if (!players_.contains(client)) {
                players_[client] = world_->create(std::format("Spieler {}", client))
                                       .set(replication::Replicated{})
                                       .set(Position{.y = 1.0f})
                                       .id();
            }
        }
        std::erase_if(players_, [&](const auto& entry) {
            if (std::ranges::find(clients, entry.first) != clients.end()) {
                return false;
            }
            world_->destroy(entry.second);
            inputs_.erase(entry.first);
            return true;
        });
    }

    void handle(const replication::Received& message) {
        if (message.data.size() == 2) {
            inputs_[message.connection] = {static_cast<i8>(message.data[0]),
                                           static_cast<i8>(message.data[1])};
        }
    }

    void update(f32 dt) {
        world_->each<Orbit, Position>([&](Orbit& orbit, Position& position) {
            orbit.angle += orbit.speed * dt;
            position = {orbit.radius * std::cos(orbit.angle), 0.0f,
                        orbit.radius * std::sin(orbit.angle)};
        });
        for (const auto& [client, entity] : players_) {
            const auto input = inputs_.find(client);
            if (input == inputs_.end()) {
                continue;
            }
            Position* position = world_->entity(entity).get_mut<Position>();
            position->x += static_cast<f32>(input->second.x) / 100.0f * player_speed * dt;
            position->z += static_cast<f32>(input->second.z) / 100.0f * player_speed * dt;
        }
    }

    [[nodiscard]] u64 player(net::ConnectionId client) const {
        const auto it = players_.find(client);
        return it == players_.end() ? 0 : it->second;
    }

private:
    world::World* world_;
    std::unordered_map<net::ConnectionId, u64> players_;
    std::unordered_map<net::ConnectionId, Input> inputs_;
};

std::unique_ptr<net::Transport> make_transport() {
    auto transport = net::Transport::create();
    if (!transport) {
        log::error("{}", transport.error());
        return nullptr;
    }
    return std::move(*transport);
}

void log_stats(const net::Transport& transport, net::ConnectionId connection,
               std::string_view who) {
    const net::ConnectionStats stats = transport.stats(connection);
    if (stats.quality < 0.0f) {
        // GameNetworkingSockets misst Qualität und Datenraten in Intervallen von einigen Sekunden.
        log::info("{}: Ping {} ms, Datenraten noch nicht gemessen", who, stats.ping_ms);
        return;
    }
    log::info("{}: Ping {} ms, Qualität {:.2f}, senden {:.0f} B/s, empfangen {:.0f} B/s", who,
              stats.ping_ms, stats.quality, stats.send_rate_bytes, stats.receive_rate_bytes);
}

} // namespace

// Server und Client im selben Prozess, über localhost.
export int run_net_demo(u64 frames) {
    Types types;
    world::World server_world{types.registry};
    world::World client_world{types.registry};
    auto server_transport = make_transport();
    auto client_transport = make_transport();
    if (!server_transport || !client_transport) {
        return 1;
    }
    const auto port = server_transport->listen(0);
    if (!port) {
        log::error("{}", port.error());
        return 1;
    }
    replication::Server server{*server_transport, server_world, types.replicated};
    replication::Client client{*client_transport, client_world, types.replicated};
    if (auto result = client.connect(std::format("127.0.0.1:{}", *port)); !result) {
        log::error("{}", result.error());
        return 1;
    }
    ServerGame game{server_world};

    std::vector<replication::Received> received;
    FrameClock clock;
    usize snapshot_bytes = 0;
    u32 snapshots = 0;
    auto server_frame = [&](u64 frame) {
        received.clear();
        server.poll(received);
        game.sync_players(server.clients());
        for (const auto& message : received) {
            game.handle(message);
        }
        game.update(frame_time);
        if (frame % snapshot_interval == 0) {
            server.send_snapshot();
            snapshot_bytes += server.last_snapshot_size();
            ++snapshots;
        }
    };
    auto client_frame = [&](u64 frame) {
        received.clear();
        if (auto result = client.poll(received); !result) {
            log::error("Client: {}", result.error());
            return false;
        }
        if (client.state() == replication::ClientState::Ready) {
            client.send(encode_input(scripted_input(frame)), net::Delivery::Unreliable);
        }
        return true;
    };

    for (u64 frame = 0; frame < frames; ++frame) {
        server_frame(frame);
        if (!client_frame(frame)) {
            return 1;
        }
        clock.wait();
    }
    if (client.state() != replication::ClientState::Ready) {
        log::error("Der Client hat keine Verbindung bekommen");
        return 1;
    }

    // Zum Vergleich ein letzter Schnappschuss ohne weitere Bewegung; warten, bis er ankommt.
    server.send_snapshot();
    const u32 final_tick = server.tick();
    for (int i = 0; i < 300 && client.mirror().last_tick() != final_tick; ++i) {
        if (!client_frame(frames)) {
            return 1;
        }
        // Geht der letzte unzuverlässige Schnappschuss verloren, kommt derselbe Stand erneut.
        if (i % 10 == 9) {
            server.send_snapshot();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    usize mismatches = 0;
    server_world.each<const replication::Replicated, const Position>(
        [&](world::Entity entity, const replication::Replicated&, const Position& expected) {
            const u64 local = client.mirror().local_id(entity.id());
            const Position* actual =
                local == 0 ? nullptr : client_world.entity(local).get<Position>();
            if (actual == nullptr || actual->x != expected.x || actual->y != expected.y ||
                actual->z != expected.z) {
                ++mismatches;
            }
        });
    const net::ConnectionId on_server = server.clients().empty() ? 0 : server.clients().front();
    const u64 player = game.player(on_server);
    const u64 mirrored_player = client.mirror().local_id(player);
    if (mirrored_player != 0) {
        const Position* position = client_world.entity(mirrored_player).get<Position>();
        log::info("Spielfigur beim Client: {} bei ({:.2f}, {:.2f}, {:.2f})",
                  client_world.entity(mirrored_player).name(), position->x, position->y,
                  position->z);
    }
    const usize average = snapshots == 0 ? 0 : snapshot_bytes / snapshots;
    log::info("{} Schnappschüsse gesendet, {} angewendet, im Mittel {} Bytes für {} Entities "
              "(etwa {:.1f} KB/s pro Client bei {} Hz)",
              snapshots, client.snapshots_applied(), average, client.mirror().entity_count(),
              static_cast<f64>(average) * 60.0 / snapshot_interval / 1000.0,
              60 / snapshot_interval);
    log_stats(*client_transport, client.connection(), "Client");
    if (mismatches != 0 || mirrored_player == 0) {
        log::error("{} Entities stimmen beim Client nicht mit dem Server überein", mismatches);
        return 1;
    }
    log::info("Server und Client stimmen überein");
    return 0;
}

// Dedizierter Server ohne Fenster und GPU. Ohne frames läuft er, bis man ihn beendet.
export int run_server(std::optional<u64> frames, std::optional<u16> port) {
    Types types;
    world::World world{types.registry};
    auto transport = make_transport();
    if (!transport) {
        return 1;
    }
    const auto bound = transport->listen(port.value_or(default_port));
    if (!bound) {
        log::error("{}", bound.error());
        return 1;
    }
    replication::Server server{*transport, world, types.replicated};
    ServerGame game{world};
    std::vector<replication::Received> received;
    FrameClock clock;
    for (u64 frame = 0; !frames || frame < *frames; ++frame) {
        received.clear();
        server.poll(received);
        game.sync_players(server.clients());
        for (const auto& message : received) {
            game.handle(message);
        }
        game.update(frame_time);
        if (frame % snapshot_interval == 0) {
            server.send_snapshot();
        }
        if (frame % 300 == 299) {
            log::info("Server: {} Client(s), {} Entities, Schnappschuss {} Bytes",
                      server.clients().size(), world.entity_count(), server.last_snapshot_size());
            for (const net::ConnectionId client : server.clients()) {
                log_stats(*transport, client, std::format("Client {}", client));
            }
        }
        clock.wait();
    }
    return 0;
}

// Client ohne Fenster: verbindet, schickt Eingaben und meldet, was ankommt. Fehlschlag, wenn
// bis zum Ende keine Schnappschüsse kamen.
export int run_client(const std::string& address, u64 frames) {
    Types types;
    world::World world{types.registry};
    auto transport = make_transport();
    if (!transport) {
        return 1;
    }
    replication::Client client{*transport, world, types.replicated};
    const std::string target = address.find(':') == std::string::npos
                                   ? std::format("{}:{}", address, default_port)
                                   : address;
    if (auto result = client.connect(target); !result) {
        log::error("{}", result.error());
        return 1;
    }
    log::info("Verbinde zu {}", target);
    std::vector<replication::Received> received;
    FrameClock clock;
    for (u64 frame = 0; frame < frames; ++frame) {
        received.clear();
        if (auto result = client.poll(received); !result) {
            log::error("{}", result.error());
            return 1;
        }
        if (client.state() == replication::ClientState::Disconnected) {
            log::error("Keine Verbindung zum Server");
            return 1;
        }
        if (client.state() == replication::ClientState::Ready) {
            client.send(encode_input(scripted_input(frame)), net::Delivery::Unreliable);
        }
        if (frame % 60 == 59 && client.state() == replication::ClientState::Ready) {
            log::info("Client: {} Entities gespiegelt, Tick {}", client.mirror().entity_count(),
                      client.mirror().last_tick());
        }
        clock.wait();
    }
    if (client.snapshots_applied() == 0) {
        log::error("Keine Schnappschüsse empfangen");
        return 1;
    }
    log_stats(*transport, client.connection(), "Client");
    log::info("{} Schnappschüsse angewendet", client.snapshots_applied());
    client.disconnect();
    // Kurz weiterpollen, damit die Trennung beim Server ankommt.
    std::vector<replication::Received> ignored;
    for (int i = 0; i < 10; ++i) {
        (void)client.poll(ignored);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return 0;
}

} // namespace sandbox
