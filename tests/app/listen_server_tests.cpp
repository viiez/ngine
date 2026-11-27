#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>

import ngine.core;
import ngine.audio;
import ngine.jobs;
import ngine.math;
import ngine.net;
import ngine.replication;
import ngine.world;
import app.game_server;
import app.game_world;
import app.net_client;

// Listen-Server (R5.5, NET-03): Server und Host-Client im selben Prozess über eine Verbindung im
// Speicher, dazu ein Client über UDP auf localhost. Beide bekommen eine Figur, spiegeln die Welt
// und erhalten Ereignisse und Einmal-Klänge der Skripte im Takt der Interpolation; mit Verzögerung
// und Verlust kommen die zuverlässigen Ereignisse trotzdem an.

using namespace ngine;

namespace {

class SimulatedNetwork {
public:
    explicit SimulatedNetwork(net::NetworkConditions conditions) {
        net::Transport::simulate(conditions);
    }
    ~SimulatedNetwork() {
        net::Transport::simulate({});
    }
    SimulatedNetwork(const SimulatedNetwork&) = delete;
    SimulatedNetwork& operator=(const SimulatedNetwork&) = delete;
};

// Kleine Welt statt des Spielplatzes: flaches Terrain um den Ursprung (32 m breit, Boden auf 10 m),
// die Figuren kommen neben den Startpunkt in der Mitte.
std::expected<math::DVec3, std::string> small_world(app::GameWorld& game) {
    game.world().create("Terrain").set(world::Terrain{.size_x = 2,
                                                      .size_y = 2,
                                                      .size_z = 2,
                                                      .shape = world::TerrainShape::Flat,
                                                      .seed = 3,
                                                      .ground = 10.0f});
    return math::DVec3{0.0, 11.0, 0.0};
}

struct Player {
    explicit Player(jobs::JobSystem* jobs) : game{{.worker_count = 1, .jobs = jobs}} {
        game.physics().set_remote(true);
        replication::register_types(game.types());
        schema = std::make_unique<replication::Schema>(game.types());
    }

    void frame() {
        REQUIRE(net->poll());
        net->client().mirror().interpolate(1.0 / 60.0);
        net->deliver();
        (void)game.update(1.0 / 60.0);
    }

    app::GameWorld game;
    std::unique_ptr<replication::Schema> schema;
    std::unique_ptr<app::NetClient> net;
};

void serve_two_players() {
    auto created = app::ListenServer::create({.load = small_world, .port = u16{0}});
    REQUIRE(created);
    app::ListenServer& host = **created;
    REQUIRE(host.session().port().has_value());

    // Der Host teilt sein Job-System nicht mit, damit der Test unabhängig bleibt.
    Player local{nullptr};
    auto in_memory =
        app::NetClient::connect_local(local.game.world(), *local.schema, host.transport());
    REQUIRE(in_memory);
    local.net = std::move(*in_memory);
    Player remote{nullptr};
    auto over_udp = app::NetClient::connect(remote.game.world(), *remote.schema,
                                            std::format("127.0.0.1:{}", *host.session().port()));
    REQUIRE(over_udp);
    remote.net = std::move(*over_udp);

    // Im Takt der Uhr, damit Verzögerung, Verlust und Wiederholungen von UDP wie im Spiel wirken.
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();
    const auto frame = [&] {
        CHECK(host.advance(1.0 / 60.0) == 1);
        local.frame();
        remote.frame();
        next += std::chrono::microseconds{16'667};
        std::this_thread::sleep_until(next);
    };
    for (int i = 0; i < 600 && !(local.net->ready() && remote.net->ready() &&
                                 local.net->figure() != 0 && remote.net->figure() != 0);
         ++i) {
        frame();
    }
    REQUIRE(local.net->ready());
    REQUIRE(remote.net->ready());
    REQUIRE(local.net->figure() != 0);
    REQUIRE(remote.net->figure() != 0);
    CHECK(host.session().players().player_count() == 2);
    CHECK(local.net->address() == "Speicher");

    // Jeder sieht beide Figuren.
    const u64 local_figure = local.net->figure_on_server();
    const u64 remote_figure = remote.net->figure_on_server();
    CHECK(local_figure != remote_figure);
    CHECK(local.net->client().mirror().local_id(remote_figure) != 0);
    CHECK(remote.net->client().mirror().local_id(local_figure) != 0);

    // Wie Net.Broadcast, Net.Send und Audio.Play eines Skripts auf dem Server.
    app::ServerSession& session = host.session();
    session.send_event(local_figure, 0, "alle", 1.0f);
    session.send_event(remote_figure, remote_figure, "privat", 2.0f);
    session.send_sound(42, math::DVec3{0.0, 11.0, 0.0}, audio::AudioBus::Effects, 1.0f, 1.0f);
    session.send_sound(43, math::DVec3{5000.0, 11.0, 0.0}, audio::AudioBus::Effects, 1.0f, 1.0f);
    CHECK(session.net_stats().events == 3);
    // Nur in Hörweite: der zweite Klang ist für niemanden.
    CHECK(session.net_stats().sounds == 2);
    for (int i = 0; i < 120; ++i) {
        frame();
    }
    CHECK(local.net->event_stats().received == 1);
    CHECK(remote.net->event_stats().received == 2);
    CHECK(local.net->event_stats().dropped == 0);
    CHECK(remote.net->event_stats().dropped == 0);
    // Beide Figuren stehen auf dem Terrain des Servers.
    for (const u64 figure : {local_figure, remote_figure}) {
        const auto* transform = host.game().world().entity(figure).get<world::Transform>();
        REQUIRE(transform != nullptr);
        CHECK(transform->position.y > 9.5);
    }

    // Ein Spieler geht, der andere bleibt.
    remote.net->disconnect();
    for (int i = 0; i < 120 && host.session().players().player_count() > 1; ++i) {
        frame();
    }
    CHECK(host.session().players().player_count() == 1);
    CHECK(local.net->ready());
}

} // namespace

TEST_CASE("Listen server serves an in-memory host and a UDP client", "[app][net]") {
    serve_two_players();
}

TEST_CASE("Listen server events arrive despite lag and loss", "[app][net]") {
    const SimulatedNetwork network{{.lag_ms = 40, .loss_percent = 5.0f}};
    serve_two_players();
}
