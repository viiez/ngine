#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.net;
import ngine.physics;
import ngine.reflect;
import ngine.replication;
import ngine.voxel;
import ngine.world;
import app.game_world;
import app.physics;

// Terrain übers Netz (R5.4, NET-06): Server und Clients im selben Prozess über localhost. Der
// Server gräbt (wie für zwei Spieler zugleich) und verteilt die Deltas mit TerrainSender; die
// Clients setzen sie mit TerrainReceiver in ihr Terrain. Eine Kiste über einer Grube fällt auf dem
// Server und in den Spiegeln, ein später beitretender Client bekommt alle Gruben.

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

// Ein Client mit eigener Welt; die Physik folgt nur dem Server (Modus remote).
struct TerrainClient {
    explicit TerrainClient(u16 port) {
        game.physics().set_remote(true);
        replication::register_types(game.types());
        schema = std::make_unique<replication::Schema>(game.types());
        auto created = net::Transport::create();
        REQUIRE(created);
        transport = std::move(*created);
        client = std::make_unique<replication::Client>(*transport, game.world(), *schema);
        REQUIRE(client->connect(std::format("127.0.0.1:{}", port)));
    }

    void frame() {
        received.clear();
        REQUIRE(client->poll(received));
        for (const replication::Received& message : received) {
            if (replication::game_message(message.data) == replication::GameMessage::Terrain) {
                REQUIRE(terrain.receive(message.data));
            }
        }
        (void)terrain.apply(game.world(), client->mirror());
        client->mirror().interpolate(1.0 / 60.0);
        (void)game.update(1.0 / 60.0);
    }

    // Eigene Kennung der Entity, die auf dem Server server_id heißt (0: noch nicht da).
    [[nodiscard]] world::Entity entity(u64 server_id) {
        const u64 local = client->mirror().local_id(server_id);
        return local != 0 && game.world().alive(local) ? game.world().entity(local)
                                                       : world::Entity{};
    }

    app::GameWorld game{{.worker_count = 1}};
    std::unique_ptr<replication::Schema> schema;
    std::unique_ptr<net::Transport> transport;
    std::unique_ptr<replication::Client> client;
    replication::TerrainReceiver terrain;
    std::vector<replication::Received> received;
};

struct TerrainSession {
    TerrainSession() {
        world::World& world = server.world();
        terrain = world.create("Terrain");
        terrain.set(world::Terrain{.size_x = 4,
                                   .size_y = 2,
                                   .size_z = 4,
                                   .shape = world::TerrainShape::Flat,
                                   .seed = 7,
                                   .ground = 10.0f});
        box = app::add_box(world, {2.0, 10.6, 2.0}, "Kiste");
        replication::register_types(server.types());
        schema = std::make_unique<replication::Schema>(server.types());
        auto created = net::Transport::create();
        REQUIRE(created);
        transport = std::move(*created);
        const auto bound = transport->listen(0);
        REQUIRE(bound);
        port = *bound;
        replication = std::make_unique<replication::Server>(*transport, world, *schema);
        const std::array<const reflect::TypeInfo*, 1> global{&server.types().get<world::Terrain>()};
        replication->set_global(global);
        // Erst stehen lassen: Terrain, Kollision und Kiste in Ruhe.
        for (int i = 0; i < 60; ++i) {
            (void)server.update(1.0 / 60.0);
        }
    }

    TerrainClient& join() {
        clients.push_back(std::make_unique<TerrainClient>(port));
        return *clients.back();
    }

    void frame() {
        received.clear();
        replication->poll(received);
        for (const net::ConnectionId id : replication->clients()) {
            if (std::ranges::find(known, id) == known.end()) {
                known.push_back(id);
                sender.add_client(id);
                // Mittelpunkt der Relevanz: wo eine Figur stünde.
                sender.set_focus(id, math::DVec3{0.0, 10.0, 0.0});
            }
        }
        (void)server.update(1.0 / 60.0);
        if (frame_++ % 2 == 0) {
            replication->send_snapshot();
            sender.update_terrain(terrain.id(), world::terrain_deltas(terrain),
                                  world::terrain_world(terrain, math::DVec3{0.0, 0.0, 0.0}));
            for (const net::ConnectionId id : known) {
                for (const auto& message : sender.encode(id, 0)) {
                    (void)replication->send(id, message, net::Delivery::Reliable);
                }
            }
        }
        for (const auto& client : clients) {
            client->frame();
        }
        next += std::chrono::microseconds{16'667};
        std::this_thread::sleep_until(next);
    }

    template <typename Done>
    bool run(u32 frames, Done&& done) {
        for (u32 i = 0; i < frames; ++i) {
            frame();
            if (done()) {
                return true;
            }
        }
        return false;
    }

    // Pinsel an einem Punkt der Welt, wie ein Spieler auf dem Server.
    void dig(math::DVec3 at, f32 radius) {
        const math::DVec3 center = world::terrain_voxels(terrain, at);
        const voxel::TerrainEdit edit = world::edit_terrain(
            terrain, {.mode = voxel::BrushMode::Remove,
                      .center = {static_cast<f32>(center.x), static_cast<f32>(center.y),
                                 static_cast<f32>(center.z)},
                      .radius = static_cast<f32>(radius / voxel::voxel_size)});
        REQUIRE_FALSE(edit.empty());
    }

    // Der Client hat dieselben Deltas wie der Server.
    [[nodiscard]] bool same_terrain(TerrainClient& client) {
        const world::Entity mirrored = client.entity(terrain.id());
        if (!mirrored) {
            return false;
        }
        const auto mine = world::terrain_deltas(mirrored);
        const auto truth = world::terrain_deltas(terrain);
        if (!mine || !truth || mine->size() != truth->size()) {
            return false;
        }
        for (const auto& [coord, delta] : *truth) {
            const auto it = mine->find(coord);
            if (it == mine->end() || !(*it->second == *delta)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] f64 box_height(TerrainClient& client) {
        const world::Entity mirrored = client.entity(box.id());
        const auto* transform = mirrored ? mirrored.get<world::Transform>() : nullptr;
        return transform != nullptr ? transform->position.y : 1e9;
    }

    app::GameWorld server{{.worker_count = 2, .deterministic = true}};
    world::Entity terrain;
    world::Entity box;
    std::unique_ptr<replication::Schema> schema;
    std::unique_ptr<net::Transport> transport;
    std::unique_ptr<replication::Server> replication;
    replication::TerrainSender sender;
    std::vector<net::ConnectionId> known;
    std::vector<replication::Received> received;
    std::vector<std::unique_ptr<TerrainClient>> clients;
    u16 port = 0;
    u64 frame_ = 0;
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();
};

void two_pits_and_a_late_joiner() {
    TerrainSession session;
    TerrainClient& a = session.join();
    TerrainClient& b = session.join();
    // Beide Clients kennen das Terrain und die Kiste, und die Umgebung ist vollständig.
    REQUIRE(session.run(600, [&] {
        return a.terrain.complete() && b.terrain.complete() && session.box_height(a) < 100.0 &&
               session.box_height(b) < 100.0;
    }));
    const f64 resting = session.box.get<world::Transform>()->position.y;

    // Zwei Spieler graben gleichzeitig: einer unter der Kiste, einer weiter weg.
    const u64 before = session.sender.stats(session.known.front()).bytes;
    session.dig({2.0, 10.0, 2.0}, 1.6f);
    session.dig({-12.0, 10.0, -10.0}, 1.2f);
    REQUIRE(session.run(300, [&] { return session.same_terrain(a) && session.same_terrain(b); }));
    // Messung: Bytes für zwei Gruben an einen Client.
    WARN("Zwei Gruben: " << session.sender.stats(session.known.front()).bytes - before << " B");

    // Die Kiste fällt auf dem Server in die Grube und bei beiden Clients mit.
    REQUIRE(session.run(300, [&] {
        const f64 server_y = session.box.get<world::Transform>()->position.y;
        return server_y < resting - 0.5 && session.box_height(a) < resting - 0.5 &&
               session.box_height(b) < resting - 0.5;
    }));

    // Ein dritter tritt später bei und sieht beide Gruben.
    TerrainClient& c = session.join();
    REQUIRE(session.run(600, [&] { return c.terrain.complete() && session.same_terrain(c); }));
    CHECK(session.same_terrain(a));
    CHECK(session.same_terrain(b));
    // Messung: Bytes für den Beitritt mit zwei Gruben.
    const replication::TerrainClientStats late = session.sender.stats(session.known.back());
    WARN("Beitritt mit zwei Gruben: " << late.join_bytes << " B in " << late.chunks << " Chunks");
    CHECK(late.complete);
}

} // namespace

TEST_CASE("Two clients see both pits, a box falls for both and a late joiner sees all",
          "[app][net]") {
    two_pits_and_a_late_joiner();
}

TEST_CASE("Terrain deltas arrive despite lag and loss", "[app][net]") {
    // 40 ms und 5 % je Senden und Empfangen: rund 80 ms pro Richtung und 10 % Verlust.
    const SimulatedNetwork network{{.lag_ms = 40, .loss_percent = 5.0f}};
    two_pits_and_a_late_joiner();
}
