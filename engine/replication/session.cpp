module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

module ngine.replication;

import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.net;
import ngine.reflect;
import ngine.world;

namespace ngine::replication {

namespace {

// Zerlegt eine Nachricht in Art und Nutzdaten; leere Nachrichten haben keine Art.
bool split(std::span<const std::byte> message, MessageKind& kind,
           std::span<const std::byte>& payload) {
    if (message.empty()) {
        return false;
    }
    kind = static_cast<MessageKind>(message[0]);
    payload = message.subspan(1);
    return true;
}

template <typename... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

} // namespace

std::vector<std::byte> application_message(std::span<const std::byte> data) {
    std::vector<std::byte> message;
    message.reserve(data.size() + 1);
    message.push_back(static_cast<std::byte>(MessageKind::Application));
    message.insert(message.end(), data.begin(), data.end());
    return message;
}

Server::Server(net::Transport& transport, world::World& world, const Schema& schema,
               EncoderSettings settings)
    : transport_{&transport}, schema_{&schema}, encoder_{world, schema, settings} {}

void Server::set_global(std::span<const reflect::TypeInfo* const> types) {
    encoder_.set_global(types);
}

void Server::set_focus(net::ConnectionId client, std::optional<math::DVec3> focus) {
    encoder_.set_focus(client, focus);
}

void Server::poll(std::vector<Received>& received) {
    NGINE_PROFILE_ZONE("replication::Server::poll");
    events_.clear();
    transport_->poll(events_);
    for (net::Event& e : events_) {
        std::visit(Overloaded{
                       [&](net::event::Connected& connected) {
                           clients_.push_back(connected.connection);
                           encoder_.add_client(connected.connection);
                           const auto welcome = encode_welcome(*schema_);
                           transport_->send(connected.connection, welcome, net::Delivery::Reliable);
                           log::info("Client {} verbunden", connected.connection);
                       },
                       [&](net::event::Disconnected& disconnected) {
                           std::erase(clients_, disconnected.connection);
                           encoder_.remove_client(disconnected.connection);
                           log::info("Client {} getrennt: {}", disconnected.connection,
                                     disconnected.reason);
                       },
                       [&](net::event::Message& message) {
                           MessageKind kind{};
                           std::span<const std::byte> payload;
                           if (!split(message.data, kind, payload)) {
                               log::warn("Leere Nachricht von Client {}", message.connection);
                           } else if (kind == MessageKind::Application) {
                               received.push_back(
                                   {message.connection, {payload.begin(), payload.end()}});
                           } else if (kind == MessageKind::Ack) {
                               if (const auto tick = decode_ack(payload)) {
                                   encoder_.acknowledge(message.connection, *tick);
                               }
                           } else {
                               log::warn("Unbekannte Nachricht von Client {}", message.connection);
                           }
                       },
                   },
                   e);
    }
}

void Server::send_snapshot() {
    NGINE_PROFILE_ZONE("replication::Server::send_snapshot");
    ++tick_;
    encoder_.capture(tick_);
    last_snapshot_size_ = 0;
    for (const net::ConnectionId client : clients_) {
        const auto snapshot = encoder_.encode(client);
        last_snapshot_size_ = std::max(last_snapshot_size_, snapshot.size());
        transport_->send(client, snapshot, net::Delivery::Unreliable);
    }
}

bool Server::send(net::ConnectionId client, std::span<const std::byte> data,
                  net::Delivery delivery) {
    return transport_->send(client, application_message(data), delivery);
}

Client::Client(net::Transport& transport, world::World& world, const Schema& schema)
    : transport_{&transport}, schema_{&schema}, mirror_{world, schema} {}

Result<void> Client::connect(std::string_view address) {
    auto connection = transport_->connect(address);
    if (!connection) {
        return std::unexpected(connection.error());
    }
    connection_ = *connection;
    state_ = ClientState::Connecting;
    return {};
}

Result<void> Client::connect_local(net::Transport& server) {
    auto connection = transport_->connect_local(server);
    if (!connection) {
        return std::unexpected(connection.error());
    }
    connection_ = *connection;
    state_ = ClientState::Connecting;
    return {};
}

Result<void> Client::poll(std::vector<Received>& received) {
    NGINE_PROFILE_ZONE("replication::Client::poll");
    events_.clear();
    transport_->poll(events_);
    Result<void> result;
    for (net::Event& e : events_) {
        std::visit(Overloaded{
                       [&](net::event::Connected& /*connected*/) {
                           log::info("Mit dem Server verbunden, warte auf Begrüßung");
                       },
                       [&](net::event::Disconnected& disconnected) {
                           log::info("Vom Server getrennt: {}", disconnected.reason);
                           state_ = ClientState::Disconnected;
                           mirror_.clear();
                       },
                       [&](net::event::Message& message) {
                           if (!result || state_ == ClientState::Disconnected) {
                               return;
                           }
                           result = handle(message.data, received);
                       },
                   },
                   e);
    }
    if (!result) {
        disconnect();
    }
    return result;
}

Result<void> Client::handle(std::span<const std::byte> message, std::vector<Received>& received) {
    MessageKind kind{};
    std::span<const std::byte> payload;
    if (!split(message, kind, payload)) {
        return std::unexpected("Leere Nachricht vom Server");
    }
    switch (kind) {
    case MessageKind::Welcome:
        if (auto check = check_welcome(payload, *schema_); !check) {
            return std::unexpected(std::format("Server passt nicht: {}", check.error()));
        }
        state_ = ClientState::Ready;
        log::info("Vom Server begrüßt, {} Komponenten werden repliziert", schema_->size());
        return {};
    case MessageKind::Snapshot: {
        // Schnappschüsse, die vor der Begrüßung ankommen (unzuverlässig, können überholen),
        // werden verworfen.
        if (state_ != ClientState::Ready) {
            return {};
        }
        auto applied = mirror_.apply(payload);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        if (*applied) {
            ++snapshots_applied_;
            // Bestätigung, damit der Server das nächste Delta gegen diesen Stand rechnet.
            transport_->send(connection_, encode_ack(mirror_.last_tick()),
                             net::Delivery::Unreliable);
            const auto now = Clock::now();
            arrivals_.push_back({now, message.size()});
            while (!arrivals_.empty() && now - arrivals_.front().time > std::chrono::seconds{1}) {
                arrivals_.pop_front();
            }
            last_snapshot_bytes_ = message.size();
            largest_snapshot_bytes_ = std::max(largest_snapshot_bytes_, message.size());
        }
        return {};
    }
    case MessageKind::Application:
        received.push_back({connection_, {payload.begin(), payload.end()}});
        return {};
    case MessageKind::Ack: break;
    }
    return std::unexpected(
        std::format("Unbekannte Nachricht vom Server (Art {})", static_cast<u32>(kind)));
}

bool Client::send(std::span<const std::byte> data, net::Delivery delivery) {
    return transport_->send(connection_, application_message(data), delivery);
}

ClientStats Client::stats() const {
    ClientStats stats;
    if (state_ != ClientState::Disconnected) {
        const net::ConnectionStats connection = transport_->stats(connection_);
        stats.ping_ms = connection.ping_ms;
        stats.loss = connection.quality >= 0.0f ? 1.0f - connection.quality : -1.0f;
        stats.send_bytes_per_second = connection.send_rate_bytes;
        stats.receive_bytes_per_second = connection.receive_rate_bytes;
    }
    stats.snapshots_per_second = static_cast<u32>(arrivals_.size());
    for (const Arrival& arrival : arrivals_) {
        stats.snapshot_bytes_per_second += arrival.bytes;
    }
    stats.last_snapshot_bytes = last_snapshot_bytes_;
    stats.largest_snapshot_bytes = largest_snapshot_bytes_;
    stats.mirror = mirror_.stats();
    return stats;
}

void Client::disconnect() {
    if (state_ != ClientState::Disconnected) {
        transport_->disconnect(connection_);
        state_ = ClientState::Disconnected;
        mirror_.clear();
    }
}

} // namespace ngine::replication
