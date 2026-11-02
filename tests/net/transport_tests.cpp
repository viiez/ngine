#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

import ngine.core;
import ngine.net;

using namespace ngine;

namespace {

std::unique_ptr<net::Transport> make_transport() {
    auto transport = net::Transport::create();
    REQUIRE(transport);
    return std::move(*transport);
}

// Fragt beide Seiten ab, bis done() gilt oder timeout vergangen ist.
bool pump(net::Transport& a, std::vector<net::Event>& a_events, net::Transport& b,
          std::vector<net::Event>& b_events, const std::function<bool()>& done,
          std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        a.poll(a_events);
        b.poll(b_events);
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

template <typename T>
usize count_events(const std::vector<net::Event>& events) {
    usize count = 0;
    for (const net::Event& e : events) {
        count += std::holds_alternative<T>(e) ? 1 : 0;
    }
    return count;
}

std::vector<std::byte> bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span{text.data(), text.size()});
    return {view.begin(), view.end()};
}

std::string text(const std::vector<std::byte>& data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

} // namespace

TEST_CASE("Transport connects over localhost and exchanges messages", "[net]") {
    auto server = make_transport();
    auto client = make_transport();
    const auto port = server->listen(0);
    REQUIRE(port);
    CHECK(*port != 0);

    const auto connection = client->connect(std::format("127.0.0.1:{}", *port));
    REQUIRE(connection);

    std::vector<net::Event> server_events;
    std::vector<net::Event> client_events;
    REQUIRE(pump(*server, server_events, *client, client_events, [&] {
        return count_events<net::event::Connected>(server_events) == 1 &&
               count_events<net::event::Connected>(client_events) == 1;
    }));
    REQUIRE(client->connections() == std::vector<net::ConnectionId>{*connection});
    REQUIRE(server->connections().size() == 1);
    const net::ConnectionId on_server = server->connections().front();

    // Zuverlässig: alle kommen an, in Reihenfolge.
    for (int i = 0; i < 100; ++i) {
        REQUIRE(client->send(*connection, bytes(std::format("Nachricht {}", i)),
                             net::Delivery::Reliable));
    }
    server_events.clear();
    REQUIRE(pump(*server, server_events, *client, client_events,
                 [&] { return count_events<net::event::Message>(server_events) == 100; }));
    int expected = 0;
    for (const net::Event& e : server_events) {
        if (const auto* message = std::get_if<net::event::Message>(&e)) {
            CHECK(message->connection == on_server);
            CHECK(text(message->data) == std::format("Nachricht {}", expected++));
        }
    }

    // Groß und zuverlässig: wird zerlegt und wieder zusammengesetzt.
    std::vector<std::byte> large(200'000);
    for (usize i = 0; i < large.size(); ++i) {
        large[i] = static_cast<std::byte>(i * 7);
    }
    REQUIRE(server->send(on_server, large, net::Delivery::Reliable));
    client_events.clear();
    REQUIRE(pump(*server, server_events, *client, client_events,
                 [&] { return count_events<net::event::Message>(client_events) == 1; }));
    for (const net::Event& e : client_events) {
        if (const auto* message = std::get_if<net::event::Message>(&e)) {
            CHECK(message->data == large);
        }
    }

    // Unzuverlässig: über localhost geht nichts verloren.
    REQUIRE(server->send(on_server, bytes("schnell"), net::Delivery::Unreliable));
    client_events.clear();
    REQUIRE(pump(*server, server_events, *client, client_events,
                 [&] { return count_events<net::event::Message>(client_events) == 1; }));

    const net::ConnectionStats stats = client->stats(*connection);
    CHECK(stats.ping_ms >= 0);

    // Trennen: der Server erfährt es, der Client meldet für sich selbst nichts.
    client->disconnect(*connection);
    CHECK(client->connections().empty());
    CHECK_FALSE(client->send(*connection, bytes("zu spät"), net::Delivery::Reliable));
    server_events.clear();
    REQUIRE(pump(*server, server_events, *client, client_events,
                 [&] { return count_events<net::event::Disconnected>(server_events) == 1; }));
    CHECK(server->connections().empty());
}

TEST_CASE("Transport reports failed connections and bad addresses", "[net]") {
    auto client = make_transport();
    CHECK_FALSE(client->connect("kein-port"));
    CHECK_FALSE(client->connect("127.0.0.1"));

    // Ein Server, der gleich wieder schließt: der Port ist dann frei und niemand antwortet.
    u16 port = 0;
    {
        auto server = make_transport();
        const auto bound = server->listen(0);
        REQUIRE(bound);
        port = *bound;
    }
    auto other = make_transport();
    const auto connection = client->connect(std::format("127.0.0.1:{}", port));
    REQUIRE(connection);
    std::vector<net::Event> events;
    std::vector<net::Event> other_events;
    // Ohne Gegenstelle bricht die Verbindung ab (ICMP oder Zeitlimit von GameNetworkingSockets);
    // für den Test reicht es, dass sie nicht als verbunden gilt.
    pump(
        *client, events, *other, other_events,
        [&] { return count_events<net::event::Disconnected>(events) == 1; },
        std::chrono::milliseconds{500});
    CHECK(count_events<net::event::Connected>(events) == 0);
    CHECK(client->connections().empty());
}

TEST_CASE("Transport listens only once", "[net]") {
    auto server = make_transport();
    REQUIRE(server->listen(0));
    CHECK_FALSE(server->listen(0));
}
