#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

import ngine.core;
import ngine.net;
import ngine.reflect;
import ngine.replication;
import ngine.world;

using namespace ngine;

namespace {

struct Position {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

struct Health {
    i32 value = 100;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    replication::register_types(types);
    types.add<Position>("Position").field("x", &Position::x).field("y", &Position::y);
    types.add<Health>("Health").field("value", &Health::value);
    return types;
}

std::array<const reflect::TypeInfo*, 3> replicated_components(const reflect::TypeRegistry& types) {
    return {&types.get<world::Name>(), &types.get<Position>(), &types.get<Health>()};
}

// Schnappschuss ohne das Byte MessageKind, wie ihn Mirror::apply erwartet.
std::vector<std::byte> payload(std::vector<std::byte> message) {
    REQUIRE_FALSE(message.empty());
    REQUIRE(message[0] == static_cast<std::byte>(replication::MessageKind::Snapshot));
    message.erase(message.begin());
    return message;
}

} // namespace

TEST_CASE("Snapshots mirror replicated entities into another world", "[replication]") {
    const auto types = make_types();
    const auto components = replicated_components(types);
    world::World server{types};
    world::World client{types};

    world::Entity player = server.create("Spieler");
    player.set(replication::Replicated{}).set(Position{1.0f, 2.0f}).set(Health{80});
    world::Entity tree = server.create("Baum");
    tree.set(replication::Replicated{}).set(Position{5.0f, 0.0f});
    world::Entity secret = server.create("Nur auf dem Server");
    secret.set(Position{9.0f, 9.0f});

    // Eine eigene Entity des Clients bleibt unberührt.
    world::Entity local = client.create("Kamera");

    replication::Mirror mirror{client, components};
    auto applied = mirror.apply(payload(replication::encode_snapshot(server, components, 1)));
    REQUIRE(applied);
    CHECK(*applied);
    CHECK(mirror.entity_count() == 2);
    CHECK(mirror.last_tick() == 1);
    CHECK(client.entity_count() == 3);
    CHECK(local.alive());

    world::Entity mirrored = client.entity(mirror.local_id(player.id()));
    REQUIRE(mirrored.alive());
    CHECK(mirrored.name() == "Spieler");
    CHECK(mirrored.get<Position>()->x == 1.0f);
    CHECK(mirrored.get<Position>()->y == 2.0f);
    CHECK(mirrored.get<Health>()->value == 80);
    CHECK(mirrored.get<replication::Replicated>()->server_id == player.id());
    CHECK(mirror.local_id(secret.id()) == 0);
    CHECK_FALSE(client.entity(mirror.local_id(tree.id())).has<Health>());

    // Änderungen, entfernte Komponenten und zerstörte Entities kommen mit dem nächsten Tick an.
    player.set(Position{3.0f, 4.0f});
    player.remove<Health>();
    tree.destroy();
    world::Entity rock = server.create("Stein");
    rock.set(replication::Replicated{}).set(Health{1});
    const auto tick2 = payload(replication::encode_snapshot(server, components, 2));
    REQUIRE(mirror.apply(tick2));
    CHECK(mirror.entity_count() == 2);
    CHECK(mirrored.get<Position>()->x == 3.0f);
    CHECK_FALSE(mirrored.has<Health>());
    CHECK(mirror.local_id(tree.id()) == 0);
    CHECK(client.entity(mirror.local_id(rock.id())).get<Health>()->value == 1);
    CHECK(client.entity_count() == 3);

    // Ein älterer Schnappschuss, der zu spät kommt, ändert nichts.
    auto stale = mirror.apply(payload(replication::encode_snapshot(server, components, 1)));
    REQUIRE(stale);
    CHECK_FALSE(*stale);
    CHECK(mirror.last_tick() == 2);

    mirror.clear();
    CHECK(client.entity_count() == 1);
    CHECK(local.alive());
}

TEST_CASE("Mirror rejects broken snapshots", "[replication]") {
    const auto types = make_types();
    const auto components = replicated_components(types);
    world::World server{types};
    world::World client{types};
    server.create("Spieler").set(replication::Replicated{}).set(Position{1.0f, 2.0f});
    const auto snapshot = payload(replication::encode_snapshot(server, components, 1));

    replication::Mirror mirror{client, components};
    // Jede Kürzung muss als Fehler enden, nicht als Absturz oder riesige Allokation.
    for (usize size = 0; size < snapshot.size(); ++size) {
        replication::Mirror fresh{client, components};
        CHECK_FALSE(fresh.apply(std::span{snapshot}.first(size)));
        fresh.clear();
    }
    auto extra = snapshot;
    extra.push_back(std::byte{0});
    CHECK_FALSE(mirror.apply(extra));
}

TEST_CASE("Welcome detects mismatched component lists", "[replication]") {
    const auto types = make_types();
    const auto components = replicated_components(types);
    auto welcome = replication::encode_welcome(components);
    REQUIRE(welcome[0] == static_cast<std::byte>(replication::MessageKind::Welcome));
    const auto body = std::span{welcome}.subspan(1);
    CHECK(replication::check_welcome(body, components));

    const std::array<const reflect::TypeInfo*, 2> fewer{components[0], components[1]};
    CHECK_FALSE(replication::check_welcome(body, fewer));
    const std::array<const reflect::TypeInfo*, 3> swapped{components[0], components[2],
                                                          components[1]};
    CHECK_FALSE(replication::check_welcome(body, swapped));
    CHECK_FALSE(replication::check_welcome(body.first(3), components));
}

TEST_CASE("Server replicates a component to a client over localhost", "[replication][net]") {
    const auto types = make_types();
    const auto components = replicated_components(types);
    world::World server_world{types};
    world::World client_world{types};

    auto server_transport = net::Transport::create();
    auto client_transport = net::Transport::create();
    REQUIRE(server_transport);
    REQUIRE(client_transport);
    const auto port = (*server_transport)->listen(0);
    REQUIRE(port);

    replication::Server server{**server_transport, server_world, components};
    replication::Client client{**client_transport, client_world, components};
    REQUIRE(client.connect(std::format("127.0.0.1:{}", *port)));

    world::Entity player = server_world.create("Spieler");
    player.set(replication::Replicated{}).set(Position{0.0f, 0.0f});

    std::vector<replication::Received> server_received;
    std::vector<replication::Received> client_received;
    auto pump = [&](auto&& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            server.poll(server_received);
            server.send_snapshot();
            REQUIRE(client.poll(client_received));
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    };

    REQUIRE(pump([&] { return client.state() == replication::ClientState::Ready; }));
    CHECK(server.clients().size() == 1);

    // Der Client schickt eine Eingabe, der Server bewegt den Spieler, der Client sieht es.
    const std::array input{std::byte{1}};
    REQUIRE(client.send(input, net::Delivery::Reliable));
    REQUIRE(pump([&] { return !server_received.empty(); }));
    CHECK(server_received.front().data == std::vector<std::byte>{std::byte{1}});
    player.set(Position{7.0f, 8.0f});

    REQUIRE(pump([&] {
        const u64 local = client.mirror().local_id(player.id());
        if (local == 0) {
            return false;
        }
        const auto* position = client_world.entity(local).get<Position>();
        return position != nullptr && position->x == 7.0f && position->y == 8.0f;
    }));
    CHECK(client.snapshots_applied() > 0);

    const std::array reply{std::byte{2}, std::byte{3}};
    REQUIRE(server.send(server.clients().front(), reply, net::Delivery::Reliable));
    REQUIRE(pump([&] { return !client_received.empty(); }));
    CHECK(client_received.front().data == std::vector<std::byte>{std::byte{2}, std::byte{3}});

    client.disconnect();
    CHECK(client.state() == replication::ClientState::Disconnected);
    CHECK(client_world.entity_count() == 0);
    REQUIRE(pump([&] { return server.clients().empty(); }));
}

TEST_CASE("Client disconnects when the server replicates other components", "[replication][net]") {
    const auto types = make_types();
    const auto components = replicated_components(types);
    const std::array<const reflect::TypeInfo*, 1> other{&types.get<Position>()};
    world::World server_world{types};
    world::World client_world{types};

    auto server_transport = net::Transport::create();
    auto client_transport = net::Transport::create();
    REQUIRE(server_transport);
    REQUIRE(client_transport);
    const auto port = (*server_transport)->listen(0);
    REQUIRE(port);

    replication::Server server{**server_transport, server_world, components};
    replication::Client client{**client_transport, client_world, other};
    REQUIRE(client.connect(std::format("127.0.0.1:{}", *port)));

    std::vector<replication::Received> received;
    bool failed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!failed && std::chrono::steady_clock::now() < deadline) {
        server.poll(received);
        failed = !client.poll(received);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(failed);
    CHECK(client.state() == replication::ClientState::Disconnected);
}
