#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

import ngine.core;
import ngine.input;
import ngine.math;
import ngine.net;
import ngine.physics;
import ngine.platform;
import ngine.replication;
import ngine.world;
import app.game_world;
import app.player;
import app.prediction;

using namespace ngine;

namespace {

// Boden als unbewegte Box, damit die Figuren ohne Terrain stehen.
void add_ground(world::World& world) {
    world.create("Boden")
        .set(world::Transform{.position = {0.0, -0.5, 0.0}})
        .set(physics::RigidBody{.motion = physics::MotionType::Static})
        .set(physics::Collider{.shape = physics::ShapeType::Box,
                               .half_extents = {60.0f, 0.5f, 60.0f}});
}

// Läuft geradeaus (W) mit der Steuerung in C++.
void walk(world::Entity figure) {
    app::apply_motion(figure, 0.0f, app::PlayerControls{.forward = 1.0f});
}

} // namespace

TEST_CASE("Character state can be set back and stepped alone", "[app][physics]") {
    app::GameWorld game{{.worker_count = 2, .deterministic = true}};
    add_ground(game.world());
    world::Entity figure = game.spawn_figure({0.0, 0.0, 0.0}, "Läufer");
    physics::PhysicsWorld& physics = game.physics();
    for (int i = 0; i < 30; ++i) {
        walk(figure);
        (void)game.update(1.0 / 60.0);
    }
    const auto start = physics.character_snapshot(figure.id());
    REQUIRE(start);
    CHECK(start->ground == physics::GroundState::Ground);
    CHECK(start->velocity.z < -1.0f);

    // Nur die Figur weiterrechnen: zwanzig Schritte, mit einem Sprung im fünften.
    const auto run = [&] {
        for (int i = 0; i < 20; ++i) {
            walk(figure);
            if (i == 5) {
                figure.get_mut<physics::CharacterMotion>()->jump = true;
            }
            REQUIRE(physics.step_character(figure.id(), 1.0f / 60.0f));
        }
        return *physics.character_snapshot(figure.id());
    };
    const physics::CharacterSnapshot first = run();
    CHECK(first.position.y > 0.1);
    CHECK(figure.get<world::Transform>()->position.z == first.position.z);

    // Zurücksetzen und dasselbe noch einmal: gleiche Lage.
    REQUIRE(physics.set_character(figure.id(), *start));
    CHECK(figure.get<world::Transform>()->position.z == start->position.z);
    const physics::CharacterSnapshot second = run();
    CHECK(math::length(first.position - second.position) < 1e-4);
    CHECK(math::length(first.velocity - second.velocity) < 1e-3f);
    CHECK_FALSE(physics.step_character(0, 1.0f / 60.0f));
    CHECK_FALSE(physics.character_snapshot(0).has_value());
}

TEST_CASE("Remote physics moves only the predicted figure", "[app][physics]") {
    app::GameWorld game{{.worker_count = 2}};
    game.physics().set_remote(true);
    add_ground(game.world());
    world::Entity own = game.spawn_figure({-2.0, 0.0, 0.0}, "Eigene");
    world::Entity other = game.spawn_figure({2.0, 0.0, 0.0}, "Fremde");
    own.set(physics::Predicted{});
    world::Entity box = game.world().create("Kiste");
    box.set(world::Transform{.position = {0.0, 3.0, -5.0}})
        .set(physics::RigidBody{})
        .set(physics::Collider{.shape = physics::ShapeType::Box});
    for (int i = 0; i < 60; ++i) {
        walk(own);
        if (other.has<physics::CharacterMotion>()) {
            walk(other);
        }
        (void)game.update(1.0 / 60.0);
    }
    CHECK(own.get<world::Transform>()->position.z < -2.0);
    // Die fremde Figur und die Kiste folgen nur ihrem Transform.
    CHECK(math::length(other.get<world::Transform>()->position - math::DVec3{2.0, 0.0, 0.0}) ==
          0.0);
    CHECK(math::length(box.get<world::Transform>()->position - math::DVec3{0.0, 3.0, -5.0}) == 0.0);
    CHECK_FALSE(game.physics().character_snapshot(other.id()).has_value());
    CHECK(game.physics().has_body(other.id()));
    CHECK(game.physics().has_body(box.id()));
    // Stellvertreter werden nicht zwischen festen Schritten gemischt.
    CHECK_FALSE(box.has<world::Interpolated>());
}

TEST_CASE("Moving light proxies do not block the predicted figure", "[app][physics]") {
    // Eine Wand, die auf dem Server dynamisch ist, vor der eigenen Figur (70 kg): In Ruhe hält sie
    // die Figur auf; bewegt (ihr Transform kommt vom Server und ändert sich) läuft die Figur durch,
    // wenn die Wand leicht ist wie eine Kugel, und nicht, wenn sie schwer ist wie eine Kiste.
    const auto run = [](f32 mass, bool moving) {
        app::GameWorld game{{.worker_count = 2}};
        game.physics().set_remote(true);
        add_ground(game.world());
        world::Entity own = game.spawn_figure({0.0, 0.0, 0.0}, "Eigene");
        own.set(physics::Predicted{});
        world::Entity wall = game.world().create("Wand");
        wall.set(world::Transform{.position = {0.0, 1.0, -3.0}})
            .set(physics::RigidBody{.mass = mass})
            .set(physics::Collider{.shape = physics::ShapeType::Box,
                                   .half_extents = {3.0f, 1.0f, 0.2f}});
        for (int i = 0; i < 120; ++i) {
            walk(own);
            if (moving) {
                auto* transform = wall.get_mut<world::Transform>();
                transform->position.x += (i % 40 < 20 ? 0.02 : -0.02);
                wall.modified<world::Transform>();
            }
            (void)game.update(1.0 / 60.0);
        }
        return own.get<world::Transform>()->position.z;
    };
    CHECK(run(5.0f, false) > -2.9);
    CHECK(run(5.0f, true) < -4.0);
    CHECK(run(20.0f, true) > -2.9);
}

namespace {

// Setzt die simulierte Netzqualität am Ende zurück, auch wenn ein Test scheitert.
struct SimulatedNetwork {
    explicit SimulatedNetwork(net::NetworkConditions conditions) {
        net::Transport::simulate(conditions);
    }
    ~SimulatedNetwork() {
        net::Transport::simulate({});
    }
    SimulatedNetwork(const SimulatedNetwork&) = delete;
    SimulatedNetwork& operator=(const SimulatedNetwork&) = delete;
};

// Feste Spur des Clients, t zu 1/60 s ab der ersten vorhergesagten Bewegung: laufen, rennen,
// zweimal springen, drehen, ducken.
std::vector<platform::Event> track_events(u64 t) {
    std::vector<platform::Event> events;
    const auto edge = [&](u64 from, u64 to, platform::Key key) {
        if (t == from) {
            events.emplace_back(platform::event::KeyPressed{key});
        } else if (t == to) {
            events.emplace_back(platform::event::KeyReleased{key});
        }
    };
    edge(0, 240, platform::Key::W);
    edge(60, 61, platform::Key::Space);
    edge(90, 180, platform::Key::Shift);
    edge(150, 151, platform::Key::Space);
    edge(200, 230, platform::Key::Ctrl);
    if (t >= 100 && t < 140) {
        events.emplace_back(platform::event::MouseMoved{.x = 0.0f, .y = 0.0f, .dx = 4.0f});
    }
    return events;
}

// Server mit einer Figur auf flachem Boden und ein Client mit Vorhersage, beide im selben Prozess
// über localhost, je ein Bild alle 1/60 s wie im Spiel. Der Server arbeitet wie der dedizierte
// Server (sandbox/game_server.cppm): ein Befehl pro Bild aus der Warteschlange, mit jedem
// Schnappschuss der Stand der Figur an den Client.
struct PredictedSession {
    PredictedSession() {
        add_ground(server.world());
        figure = server.spawn_figure({0.0, 0.0, 0.0}, "Spieler");
        server.bind_player(figure, server_actions);
        replication::register_types(server.types());
        replication::register_types(client.types());
        server_schema = std::make_unique<replication::Schema>(server.types());
        client_schema = std::make_unique<replication::Schema>(client.types());
        auto st = net::Transport::create();
        auto ct = net::Transport::create();
        REQUIRE(st);
        REQUIRE(ct);
        server_transport = std::move(*st);
        client_transport = std::move(*ct);
        const auto port = server_transport->listen(0);
        REQUIRE(port);
        replication_server = std::make_unique<replication::Server>(*server_transport,
                                                                   server.world(), *server_schema);
        replication_client = std::make_unique<replication::Client>(*client_transport,
                                                                   client.world(), *client_schema);
        REQUIRE(replication_client->connect(std::format("127.0.0.1:{}", *port)));
        prediction = std::make_unique<app::PlayerPrediction>(
            client, client_actions, [this](std::span<const std::byte> data) {
                (void)replication_client->send(data, net::Delivery::Unreliable);
            });
    }
    ~PredictedSession() {
        server.unbind_player(figure);
    }

    // Ein Bild auf beiden Seiten; events sind die Eingaben des Clients in diesem Bild.
    void frame(std::span<const platform::Event> events) {
        // Server.
        received.clear();
        replication_server->poll(received);
        if (!possessed && !replication_server->clients().empty()) {
            (void)replication_server->send(replication_server->clients().front(),
                                           replication::encode_possess(figure.id()),
                                           net::Delivery::Reliable);
            possessed = true;
        }
        for (const replication::Received& message : received) {
            auto commands = replication::decode_commands(message.data, map.hash());
            if (!commands) {
                continue;
            }
            for (replication::Command& command : *commands) {
                (void)replication::limit_command(command, map, app::max_pitch);
                queue.push(std::move(command));
            }
        }
        if (auto command = queue.next()) {
            latest = command->actions;
            auto* input = figure.get_mut<app::PlayerInput>();
            input->yaw = command->yaw;
            input->pitch = command->pitch;
        } else {
            for (input::ActionValue& value : latest) {
                value.pressed = false;
                value.released = false;
            }
        }
        server_actions.apply(latest);
        server.steer_players();
        (void)server.update(1.0 / 60.0);
        if (server_frame++ % 2 == 0) {
            replication_server->send_snapshot();
            if (!replication_server->clients().empty() && queue.last() != 0) {
                const auto snapshot = server.physics().character_snapshot(figure.id());
                REQUIRE(snapshot);
                const replication::OwnerState owner{.command = queue.last(),
                                                    .queued = static_cast<u8>(queue.size()),
                                                    .position = snapshot->position,
                                                    .velocity = snapshot->velocity,
                                                    .ground = static_cast<u8>(snapshot->ground),
                                                    .crouched = snapshot->crouched,
                                                    .touching = snapshot->touching};
                (void)replication_server->send(replication_server->clients().front(),
                                               replication::encode_owner(owner),
                                               net::Delivery::Unreliable);
            }
        }

        // Client.
        received.clear();
        REQUIRE(replication_client->poll(received));
        for (const replication::Received& message : received) {
            if (auto possess = replication::decode_possess(message.data)) {
                figure_on_server = *possess;
            } else if (auto owner = replication::decode_owner(message.data)) {
                prediction->handle_owner(*owner);
            }
        }
        replication::Mirror& mirror = replication_client->mirror();
        prediction->set_figure(figure_on_server != 0 ? mirror.local_id(figure_on_server) : 0,
                               figure_on_server, mirror);
        mirror.interpolate(1.0 / 60.0);
        client_actions.begin_frame();
        for (const platform::Event& event : events) {
            client_actions.handle(event);
        }
        client_actions.end_frame(1.0f / 60.0f);
        prediction->look(client_actions);
        (void)client.update(1.0 / 60.0);
        prediction->update(1.0f / 60.0f);

        next += std::chrono::microseconds{16'667};
        std::this_thread::sleep_until(next);
    }

    // Bilder ohne Eingabe, bis done() gilt oder frames vorbei sind.
    template <typename Done>
    bool idle(u32 frames, Done&& done) {
        for (u32 i = 0; i < frames; ++i) {
            frame({});
            if (done()) {
                return true;
            }
        }
        return false;
    }

    // Lage der Figur auf dem Client (vorhergesagt) und auf dem Server.
    [[nodiscard]] math::DVec3 client_position() {
        return client.world().entity(prediction->figure()).get<world::Transform>()->position;
    }
    [[nodiscard]] math::DVec3 server_position() {
        return figure.get<world::Transform>()->position;
    }

    input::ActionMap map = input::ActionMap::standard();
    app::GameWorld server{{.worker_count = 2, .deterministic = true}};
    app::GameWorld client{{.worker_count = 2}};
    world::Entity figure;
    input::ActionState server_actions{input::ActionMap::standard()};
    input::ActionState client_actions{input::ActionMap::standard()};
    std::unique_ptr<replication::Schema> server_schema;
    std::unique_ptr<replication::Schema> client_schema;
    std::unique_ptr<net::Transport> server_transport;
    std::unique_ptr<net::Transport> client_transport;
    std::unique_ptr<replication::Server> replication_server;
    std::unique_ptr<replication::Client> replication_client;
    std::unique_ptr<app::PlayerPrediction> prediction;
    std::vector<replication::Received> received;
    replication::CommandQueue queue;
    std::vector<input::ActionValue> latest;
    bool possessed = false;
    u64 figure_on_server = 0;
    u64 server_frame = 0;
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();
};

// Fährt die feste Spur, wartet, bis die Figur auf beiden Seiten steht, und liefert die Zahl der
// Korrekturen während der Spur.
u64 run_track(PredictedSession& session) {
    REQUIRE(session.idle(600, [&] {
        return session.prediction->figure() != 0 &&
               session.client.physics().character_snapshot(session.prediction->figure());
    }));
    // Erst stehen beide still und gleich.
    REQUIRE(session.idle(300, [&] {
        return session.prediction->report().predictor.acknowledged > 30 &&
               math::length(session.client_position() - session.server_position()) < 0.01;
    }));
    const u64 before = session.prediction->report().predictor.corrections;
    const math::DVec3 start = session.server_position();
    for (u64 t = 0; t < 260; ++t) {
        session.frame(track_events(t));
    }
    const u64 during = session.prediction->report().predictor.corrections - before;
    // Bis der Server alle Befehle der Spur verarbeitet hat und die Figur steht. Offen bleiben immer
    // einige (Puffer des Servers und Rundlaufzeit), daher zählt die Nummer des letzten.
    const u64 last = session.prediction->report().commands;
    const bool settled = session.idle(300, [&] {
        const app::PredictionReport report = session.prediction->report();
        return report.predictor.acknowledged >= last &&
               math::length(session.client_position() - session.server_position()) < 0.01 &&
               report.smoothing == 0.0f;
    });
    INFO(app::describe_prediction(session.prediction->report()));
    INFO("Abstand " << math::length(session.client_position() - session.server_position()));
    REQUIRE(settled);
    CHECK(math::length(session.server_position() - start) > 10.0);
    return during;
}

} // namespace

TEST_CASE("Client and server end at the same place after a fixed track", "[app][net]") {
    PredictedSession session;
    const u64 corrections = run_track(session);
    INFO(app::describe_prediction(session.prediction->report()));
    CHECK(corrections <= 2);
    CHECK(math::length(session.client_position() - session.server_position()) < 0.01);

    // Ein Stoß auf dem Server (Figur versetzt): eine Korrektur, weich abgebaut.
    const u64 before = session.prediction->report().predictor.corrections;
    auto* transform = session.figure.get_mut<world::Transform>();
    transform->position.x += 1.0;
    session.figure.modified<world::Transform>();
    REQUIRE(session.idle(120, [&] {
        return session.prediction->report().predictor.corrections > before &&
               session.prediction->report().smoothing > 0.0f;
    }));
    REQUIRE(session.idle(120, [&] {
        return session.prediction->report().smoothing == 0.0f &&
               math::length(session.client_position() - session.server_position()) < 0.01;
    }));
    CHECK(session.prediction->report().predictor.corrections - before <= 2);
}

TEST_CASE("Prediction holds with lag and loss", "[app][net]") {
    // Beide Seiten im selben Prozess: 40 ms und 5 % je Senden und Empfangen ergeben rund 80 ms
    // pro Richtung und 10 % Verlust.
    const SimulatedNetwork network{{.lag_ms = 40, .loss_percent = 5.0f}};
    PredictedSession session;
    const u64 corrections = run_track(session);
    INFO(app::describe_prediction(session.prediction->report()));
    CHECK(corrections <= 10);
    CHECK(math::length(session.client_position() - session.server_position()) < 0.01);
}
