module;

#include <ngine/profile.hpp>

#include <algorithm>
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
import ngine.profile;
import ngine.net;
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

Server::Server(net::Transport& transport, world::World& world, ComponentList components)
    : transport_{&transport}, world_{&world}, components_{components.begin(), components.end()} {}

void Server::poll(std::vector<Received>& received) {
    NGINE_PROFILE_ZONE("replication::Server::poll");
    events_.clear();
    transport_->poll(events_);
    for (net::Event& e : events_) {
        std::visit(
            Overloaded{
                [&](net::event::Connected& connected) {
                    clients_.push_back(connected.connection);
                    const auto welcome = encode_welcome(components_);
                    transport_->send(connected.connection, welcome, net::Delivery::Reliable);
                    log::info("Client {} verbunden", connected.connection);
                },
                [&](net::event::Disconnected& disconnected) {
                    std::erase(clients_, disconnected.connection);
                    log::info("Client {} getrennt: {}", disconnected.connection,
                              disconnected.reason);
                },
                [&](net::event::Message& message) {
                    MessageKind kind{};
                    std::span<const std::byte> payload;
                    if (split(message.data, kind, payload) && kind == MessageKind::Application) {
                        received.push_back({message.connection, {payload.begin(), payload.end()}});
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
    const auto snapshot = encode_snapshot(*world_, components_, tick_);
    last_snapshot_size_ = snapshot.size();
    for (const net::ConnectionId client : clients_) {
        transport_->send(client, snapshot, net::Delivery::Unreliable);
    }
}

bool Server::send(net::ConnectionId client, std::span<const std::byte> data,
                  net::Delivery delivery) {
    return transport_->send(client, application_message(data), delivery);
}

Client::Client(net::Transport& transport, world::World& world, ComponentList components)
    : transport_{&transport}, components_{components.begin(), components.end()},
      mirror_{world, components} {}

Result<void> Client::connect(std::string_view address) {
    auto connection = transport_->connect(address);
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
        if (auto check = check_welcome(payload, components_); !check) {
            return std::unexpected(std::format("Server passt nicht: {}", check.error()));
        }
        state_ = ClientState::Ready;
        log::info("Vom Server begrüßt, {} Komponenten werden repliziert", components_.size());
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
        snapshots_applied_ += *applied ? 1 : 0;
        return {};
    }
    case MessageKind::Application:
        received.push_back({connection_, {payload.begin(), payload.end()}});
        return {};
    }
    return std::unexpected(
        std::format("Unbekannte Nachricht vom Server (Art {})", static_cast<u32>(kind)));
}

bool Client::send(std::span<const std::byte> data, net::Delivery delivery) {
    return transport_->send(connection_, application_message(data), delivery);
}

void Client::disconnect() {
    if (state_ != ClientState::Disconnected) {
        transport_->disconnect(connection_);
        state_ = ClientState::Disconnected;
        mirror_.clear();
    }
}

} // namespace ngine::replication
