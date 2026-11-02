module;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// Adapter für GameNetworkingSockets. Die Bibliothek ist ein Singleton pro Prozess; jeder Transport
// hält eine Referenz auf die Initialisierung und bekommt die Statusmeldungen seiner Verbindungen
// über die Benutzerdaten der Verbindung zurück.
module ngine.net;

import ngine.core;
import ngine.profile;

namespace ngine::net {

namespace {

// Initialisierung mit Referenzzählung: die erste Instanz startet die Bibliothek, die letzte
// beendet sie.
std::mutex library_mutex;
u32 library_users = 0;

void debug_output(ESteamNetworkingSocketsDebugOutputType type, const char* message) {
    if (type <= k_ESteamNetworkingSocketsDebugOutputType_Error) {
        log::error("Netzwerk: {}", message);
    } else if (type <= k_ESteamNetworkingSocketsDebugOutputType_Warning) {
        log::warn("Netzwerk: {}", message);
    } else {
        log::debug("Netzwerk: {}", message);
    }
}

Result<void> acquire_library() {
    const std::scoped_lock lock{library_mutex};
    if (library_users++ > 0) {
        return {};
    }
    SteamNetworkingErrMsg error{};
    if (!GameNetworkingSockets_Init(nullptr, error)) {
        library_users = 0;
        return std::unexpected(std::format("GameNetworkingSockets startet nicht: {}", error));
    }
    ISteamNetworkingUtils* utils = SteamNetworkingUtils();
    utils->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, debug_output);
    // Ohne Steam gibt es keine Zertifikate; die Verbindung ist trotzdem verschlüsselt, nur die
    // Gegenseite ist nicht beglaubigt (ADR 0024).
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
    return {};
}

void release_library() {
    const std::scoped_lock lock{library_mutex};
    if (--library_users == 0) {
        GameNetworkingSockets_Kill();
    }
}

void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info);

} // namespace

struct Transport::Impl {
    ISteamNetworkingSockets* sockets = nullptr;
    HSteamListenSocket listen_socket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group = k_HSteamNetPollGroup_Invalid;
    // Verbindungen, die event::Connected gemeldet haben.
    std::unordered_set<HSteamNetConnection> connected;
    // Alle offenen Verbindungen dieses Transports (auch solche im Aufbau).
    std::unordered_set<HSteamNetConnection> open;
    std::vector<Event> pending;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (sockets == nullptr) {
            return;
        }
        for (const HSteamNetConnection connection : open) {
            sockets->CloseConnection(connection, 0, "Transport beendet", false);
        }
        if (listen_socket != k_HSteamListenSocket_Invalid) {
            sockets->CloseListenSocket(listen_socket);
        }
        if (poll_group != k_HSteamNetPollGroup_Invalid) {
            sockets->DestroyPollGroup(poll_group);
        }
        release_library();
    }

    void status_changed(const SteamNetConnectionStatusChangedCallback_t& info) {
        const HSteamNetConnection connection = info.m_hConn;
        switch (info.m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            // Nur eingehende Verbindungen (mit Listen-Socket) müssen angenommen werden.
            if (info.m_info.m_hListenSocket != k_HSteamListenSocket_Invalid &&
                info.m_info.m_hListenSocket == listen_socket) {
                if (sockets->AcceptConnection(connection) != k_EResultOK) {
                    sockets->CloseConnection(connection, 0, "Annehmen fehlgeschlagen", false);
                    return;
                }
                sockets->SetConnectionPollGroup(connection, poll_group);
                open.insert(connection);
            }
            break;
        case k_ESteamNetworkingConnectionState_Connected:
            if (connected.insert(connection).second) {
                pending.emplace_back(event::Connected{connection});
            }
            break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            const bool was_connected = connected.erase(connection) > 0;
            const bool was_open = open.erase(connection) > 0;
            sockets->CloseConnection(connection, 0, nullptr, false);
            if (was_connected ||
                (was_open && info.m_info.m_hListenSocket == k_HSteamListenSocket_Invalid)) {
                pending.emplace_back(event::Disconnected{connection, info.m_info.m_szEndDebug});
            }
            break;
        }
        default: break;
        }
    }
};

namespace {

void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
    auto* impl =
        reinterpret_cast<Transport::Impl*>(static_cast<std::intptr_t>(info->m_info.m_nUserData));
    if (impl != nullptr) {
        impl->status_changed(*info);
    }
}

std::array<SteamNetworkingConfigValue_t, 2> connection_options(Transport::Impl* impl) {
    std::array<SteamNetworkingConfigValue_t, 2> values{};
    values[0].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
                       static_cast<int64>(reinterpret_cast<std::intptr_t>(impl)));
    values[1].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                     reinterpret_cast<void*>(&on_status_changed));
    return values;
}

} // namespace

Result<std::unique_ptr<Transport>> Transport::create() {
    if (auto result = acquire_library(); !result) {
        return std::unexpected(result.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->sockets = SteamNetworkingSockets();
    // Alle Verbindungen (eingehend wie ausgehend) landen in einer Poll-Gruppe, damit poll()
    // Nachrichten mit einem Aufruf abholt.
    impl->poll_group = impl->sockets->CreatePollGroup();
    return std::make_unique<Transport>(std::move(impl));
}

Transport::Transport(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {
    verify(impl_ != nullptr, "Transport nur über create() erzeugen");
}

Transport::~Transport() = default;

Result<u16> Transport::listen(u16 port) {
    if (impl_->listen_socket != k_HSteamListenSocket_Invalid) {
        return std::unexpected("Der Transport lauscht schon");
    }
    const auto options = connection_options(impl_.get());
    auto try_port = [&](u16 candidate) {
        SteamNetworkingIPAddr address{};
        address.Clear();
        address.m_port = candidate;
        impl_->listen_socket = impl_->sockets->CreateListenSocketIP(
            address, static_cast<int>(options.size()), options.data());
        return impl_->listen_socket != k_HSteamListenSocket_Invalid;
    };
    bool bound = false;
    if (port != 0) {
        bound = try_port(port);
    } else {
        // GameNetworkingSockets kann den Port nicht vom System wählen lassen. Deshalb zufällige
        // Ports aus dem dynamischen Bereich (49152 bis 65535), bis einer frei ist.
        std::random_device seed;
        std::minstd_rand random{seed()};
        std::uniform_int_distribution<u32> dynamic_port{49152, 65535};
        for (int attempt = 0; attempt < 32 && !bound; ++attempt) {
            port = static_cast<u16>(dynamic_port(random));
            bound = try_port(port);
        }
    }
    if (!bound) {
        return std::unexpected(std::format("Port {} lässt sich nicht öffnen", port));
    }
    log::info("Server lauscht auf Port {}", port);
    return port;
}

Result<ConnectionId> Transport::connect(std::string_view address) {
    SteamNetworkingIPAddr target{};
    const std::string text{address};
    if (!target.ParseString(text.c_str()) || target.m_port == 0) {
        return std::unexpected(
            std::format("Ungültige Adresse: {} (erwartet adresse:port)", address));
    }
    const auto options = connection_options(impl_.get());
    const HSteamNetConnection connection = impl_->sockets->ConnectByIPAddress(
        target, static_cast<int>(options.size()), options.data());
    if (connection == k_HSteamNetConnection_Invalid) {
        return std::unexpected(std::format("Verbindung zu {} lässt sich nicht aufbauen", address));
    }
    impl_->sockets->SetConnectionPollGroup(connection, impl_->poll_group);
    impl_->open.insert(connection);
    return connection;
}

bool Transport::send(ConnectionId connection, std::span<const std::byte> data, Delivery delivery) {
    if (!impl_->connected.contains(connection)) {
        return false;
    }
    const int flags = delivery == Delivery::Reliable ? k_nSteamNetworkingSend_Reliable
                                                     : k_nSteamNetworkingSend_UnreliableNoDelay;
    return impl_->sockets->SendMessageToConnection(connection, data.data(),
                                                   static_cast<uint32>(data.size()), flags,
                                                   nullptr) == k_EResultOK;
}

void Transport::disconnect(ConnectionId connection) {
    const bool was_open = impl_->open.erase(connection) > 0;
    const bool was_connected = impl_->connected.erase(connection) > 0;
    if (was_open || was_connected) {
        // Mit Nachlauf: Zuverlässige Nachrichten, die noch unterwegs sind, kommen noch an.
        impl_->sockets->CloseConnection(connection, 0, "Verbindung beendet", true);
    }
}

void Transport::poll(std::vector<Event>& events) {
    NGINE_PROFILE_ZONE("net::Transport::poll");
    // Rückrufe laufen hier, synchron in diesem Thread; sie können auch Verbindungen anderer
    // Transporte im Prozess betreffen und landen dann in deren pending.
    impl_->sockets->RunCallbacks();
    for (Event& e : impl_->pending) {
        events.push_back(std::move(e));
    }
    impl_->pending.clear();

    auto take = [&](SteamNetworkingMessage_t* message) {
        const auto* bytes = static_cast<const std::byte*>(message->m_pData);
        events.emplace_back(event::Message{
            message->m_conn, std::vector<std::byte>{bytes, bytes + message->m_cbSize}});
        message->Release();
    };
    std::array<SteamNetworkingMessage_t*, 64> messages{};
    auto receive = [&](auto&& fetch) {
        for (;;) {
            const int count = fetch(messages.data(), static_cast<int>(messages.size()));
            for (int i = 0; i < count; ++i) {
                take(messages[static_cast<usize>(i)]);
            }
            if (count < static_cast<int>(messages.size())) {
                break;
            }
        }
    };
    receive([&](SteamNetworkingMessage_t** out, int max) {
        return impl_->sockets->ReceiveMessagesOnPollGroup(impl_->poll_group, out, max);
    });
}

std::vector<ConnectionId> Transport::connections() const {
    std::vector<ConnectionId> result{impl_->connected.begin(), impl_->connected.end()};
    std::ranges::sort(result);
    return result;
}

ConnectionStats Transport::stats(ConnectionId connection) const {
    SteamNetConnectionRealTimeStatus_t status{};
    if (impl_->sockets->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) !=
        k_EResultOK) {
        return {};
    }
    return {status.m_nPing, status.m_flConnectionQualityLocal, status.m_flOutBytesPerSec,
            status.m_flInBytesPerSec};
}

} // namespace ngine::net
