#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import ngine.core;
import ngine.input;
import ngine.math;
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
    // Nur auf dem Server: wird nicht übertragen.
    i32 regeneration = 0;
};

// Verweist auf eine andere Entity (reflect::Hint::Entity).
struct Follow {
    u64 target = 0;
};

// Nicht repliziert.
struct Secret {
    i32 code = 0;
};

reflect::TypeRegistry make_types(bool extra_field = false) {
    reflect::TypeRegistry types;
    world::register_types(types);
    replication::register_types(types);
    types.add<Position>("Position").replicated().field("x", &Position::x).field("y", &Position::y);
    if (extra_field) {
        types.add<Health>("Health")
            .replicated()
            .field("value", &Health::value)
            .field("regeneration", &Health::regeneration);
    } else {
        types.add<Health>("Health")
            .replicated()
            .field("value", &Health::value)
            .field("regeneration", &Health::regeneration)
            .local();
    }
    types.add<Follow>("Follow")
        .replicated()
        .field("target", &Follow::target)
        .hint(reflect::Hint::Entity);
    types.add<Secret>("Secret").field("code", &Secret::code);
    return types;
}

// Schnappschuss ohne das Byte MessageKind, wie ihn Mirror::apply erwartet.
std::vector<std::byte> payload(std::vector<std::byte> message) {
    REQUIRE_FALSE(message.empty());
    REQUIRE(message[0] == static_cast<std::byte>(replication::MessageKind::Snapshot));
    message.erase(message.begin());
    return message;
}

// Gleiche Stände: dieselben Entities mit denselben Elternteilen, Komponenten und Teilen.
bool same_view(const replication::View& a, const replication::View& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        const replication::EntityRecord& x = *a[i];
        const replication::EntityRecord& y = *b[i];
        if (x.id != y.id || x.parent != y.parent || x.components != y.components ||
            x.part_ends.size() != y.part_ends.size()) {
            return false;
        }
        for (usize part = 0; part < x.part_ends.size(); ++part) {
            const auto p = x.part(part);
            const auto q = y.part(part);
            if (p.size() != q.size() || !std::equal(p.begin(), p.end(), q.begin())) {
                return false;
            }
        }
    }
    return true;
}

// Server-Welt, ein Client und sein Spiegel ohne Netzwerk.
struct Link {
    explicit Link(replication::EncoderSettings settings = {})
        : types{make_types()}, schema{types}, server{types}, client{types},
          encoder{server, schema, settings}, mirror{client, schema} {
        encoder.add_client(1);
    }

    // Ein Schnappschuss: lesen, kodieren, anwenden und (wenn ack) bestätigen. Liefert die Größe.
    usize step(bool ack = true) {
        world::update_transforms(server);
        encoder.capture(++tick);
        const auto message = encoder.encode(1);
        auto applied = mirror.apply(payload(message));
        REQUIRE(applied);
        if (ack && *applied) {
            encoder.acknowledge(1, tick);
        }
        return message.size();
    }

    // Ein Schnappschuss, der verloren geht.
    void lose() {
        world::update_transforms(server);
        encoder.capture(++tick);
        (void)encoder.encode(1);
    }

    world::Entity on_client(const world::Entity& entity) {
        return client.entity(mirror.local_id(entity.id()));
    }

    reflect::TypeRegistry types;
    replication::Schema schema;
    world::World server;
    world::World client;
    replication::SnapshotEncoder encoder;
    replication::Mirror mirror;
    u32 tick = 0;
};

world::Transform at(f64 x, f64 y = 0.0, f64 z = 0.0) {
    world::Transform transform;
    transform.position = {x, y, z};
    return transform;
}

} // namespace

TEST_CASE("Schema takes replicated types and skips local fields", "[replication]") {
    const auto types = make_types();
    const replication::Schema schema{types};
    CHECK(schema.index_of(types.get<world::Name>()).has_value());
    CHECK(schema.index_of(types.get<Position>()).has_value());
    CHECK_FALSE(schema.index_of(types.get<Secret>()).has_value());
    CHECK_FALSE(schema.index_of(types.get<replication::Replicated>()).has_value());
    CHECK_FALSE(schema.index_of(types.get<world::WorldTransform>()).has_value());

    const auto health = schema.index_of(types.get<Health>());
    REQUIRE(health);
    CHECK(schema.component(*health).parts == 1);
    const auto transform = schema.transform_index();
    REQUIRE(transform);
    CHECK(schema.component(*transform).codec == replication::Codec::Transform);
    CHECK(schema.component(*transform).parts == 3);

    // Ein Feld mehr ändert den Hash.
    const auto other_types = make_types(true);
    const replication::Schema other{other_types};
    CHECK(other.hash() != schema.hash());
    CHECK(replication::Schema{make_types()}.hash() == schema.hash());
}

TEST_CASE("Snapshots mirror replicated entities into another world", "[replication]") {
    Link link;
    world::Entity player = link.server.create("Spieler");
    player.set(Position{1.0f, 2.0f}).set(Health{80, 5}).set(Secret{7});
    world::Entity tree = link.server.create("Baum");
    tree.set(Position{5.0f, 0.0f});
    // Ohne replizierte Komponente bleibt eine Entity auf dem Server.
    world::Entity hidden = link.server.create();
    hidden.set(Secret{1});
    // Eine eigene Entity des Clients bleibt unberührt.
    world::Entity local = link.client.create("Kamera");

    link.step();
    CHECK(link.mirror.entity_count() == 2);
    CHECK(link.mirror.last_tick() == 1);
    CHECK(link.client.entity_count() == 3);
    CHECK(local.alive());

    world::Entity mirrored = link.on_client(player);
    REQUIRE(mirrored.alive());
    CHECK(mirrored.name() == "Spieler");
    CHECK(mirrored.get<Position>()->x == 1.0f);
    CHECK(mirrored.get<Position>()->y == 2.0f);
    CHECK(mirrored.get<Health>()->value == 80);
    CHECK(mirrored.get<Health>()->regeneration == 0);
    CHECK_FALSE(mirrored.has<Secret>());
    CHECK(mirrored.get<replication::Replicated>()->server_id == player.id());
    CHECK(link.mirror.local_id(hidden.id()) == 0);
    CHECK_FALSE(link.on_client(tree).has<Health>());

    // Änderungen, entfernte Komponenten und zerstörte Entities kommen mit dem nächsten Tick an.
    player.set(Position{3.0f, 4.0f});
    player.remove<Health>();
    tree.destroy();
    world::Entity rock = link.server.create("Stein");
    rock.set(Health{1});
    link.step();
    CHECK(link.mirror.entity_count() == 2);
    CHECK(mirrored.get<Position>()->x == 3.0f);
    CHECK_FALSE(mirrored.has<Health>());
    CHECK(link.mirror.local_id(tree.id()) == 0);
    CHECK(link.on_client(rock).get<Health>()->value == 1);
    CHECK(link.client.entity_count() == 3);
    CHECK(same_view(link.mirror.view(), link.encoder.captured()));

    link.mirror.clear();
    CHECK(link.client.entity_count() == 1);
    CHECK(local.alive());
}

TEST_CASE("Deltas send only changed parts", "[replication]") {
    Link link;
    world::Entity player = link.server.create("Spieler");
    player.set(Position{1.0f, 2.0f}).set(Health{80}).set(at(10.0, 2.0, 3.0));
    const usize full = link.step();

    // Nichts geändert: Kopf ohne Einträge (Art, Tick, Basis, zwei leere Anzahlen).
    CHECK(link.step() == 11);

    // Ein Feld geändert: viel kleiner als der volle Stand.
    player.set(Health{70});
    const usize delta = link.step();
    CHECK(delta < full);
    CHECK(delta < 11 + 8 + 1 + 1 + 2 + 1 + 1 + 4 + 2);
    CHECK(link.on_client(player).get<Health>()->value == 70);

    // Nur das Feld regeneration ist lokal: Es zu ändern schickt nichts.
    player.set(Health{70, 9});
    CHECK(link.step() == 11);
    CHECK(link.encoder.stats(1).full == 1);
    CHECK(same_view(link.mirror.view(), link.encoder.captured()));
}

TEST_CASE("Mirror drops snapshots with unknown base or out of order", "[replication]") {
    Link link;
    world::Entity player = link.server.create("Spieler");
    player.set(Position{1.0f, 2.0f});
    world::update_transforms(link.server);
    link.encoder.capture(1);
    const auto first = payload(link.encoder.encode(1));
    // Der Server hält Tick 1 für bestätigt; der Client hat ihn nie bekommen.
    link.encoder.acknowledge(1, 1);
    player.set(Position{5.0f, 6.0f});
    link.encoder.capture(2);
    const auto second = payload(link.encoder.encode(1));

    auto applied = link.mirror.apply(second);
    REQUIRE(applied);
    CHECK_FALSE(*applied);
    CHECK(link.mirror.stats().missing_base == 1);
    CHECK(link.mirror.entity_count() == 0);

    applied = link.mirror.apply(first);
    REQUIRE(applied);
    CHECK(*applied);
    applied = link.mirror.apply(second);
    REQUIRE(applied);
    CHECK(*applied);
    CHECK(link.on_client(player).get<Position>()->x == 5.0f);

    // Zu spät: älter als der zuletzt angewendete.
    applied = link.mirror.apply(first);
    REQUIRE(applied);
    CHECK_FALSE(*applied);
    CHECK(link.mirror.stats().stale == 1);
    CHECK(link.mirror.last_tick() == 2);
}

TEST_CASE("Mirror rejects broken snapshots", "[replication]") {
    Link link;
    link.server.create("Spieler").set(Position{1.0f, 2.0f}).set(at(1.0));
    link.encoder.capture(1);
    const auto snapshot = payload(link.encoder.encode(1));

    // Jede Kürzung muss als Fehler enden, nicht als Absturz oder riesige Allokation.
    for (usize size = 0; size < snapshot.size(); ++size) {
        replication::Mirror fresh{link.client, link.schema};
        CHECK_FALSE(fresh.apply(std::span{snapshot}.first(size)));
        fresh.clear();
    }
    auto extra = snapshot;
    extra.push_back(std::byte{0});
    CHECK_FALSE(link.mirror.apply(extra));
}

TEST_CASE("Transform codec keeps millimetres and small angles", "[replication]") {
    Link link;
    world::Entity near = link.server.create("Nah");
    world::Entity far = link.server.create("Fern");
    world::Transform a = at(12.3456, -0.0004, 63.9996);
    a.rotation = math::angle_axis(2.0f, math::normalize(math::Vec3{1.0f, 2.0f, 3.0f}));
    a.scale = {1.5f, 2.0f, 0.25f};
    near.set(a);
    const world::Transform b = at(1234567.891, -98765.4321, 5e6);
    far.set(b);
    link.step();

    const auto* x = link.on_client(near).get<world::Transform>();
    REQUIRE(x != nullptr);
    CHECK(std::abs(x->position.x - a.position.x) <= 0.0005);
    CHECK(std::abs(x->position.y - a.position.y) <= 0.0005);
    CHECK(std::abs(x->position.z - a.position.z) <= 0.0005);
    CHECK(x->scale.x == a.scale.x);
    CHECK(x->scale.z == a.scale.z);
    // Gleiche Drehung bis auf das Vorzeichen: |Skalarprodukt| nahe 1.
    const f32 dot = std::abs(x->rotation.x * a.rotation.x + x->rotation.y * a.rotation.y +
                             x->rotation.z * a.rotation.z + x->rotation.w * a.rotation.w);
    CHECK(dot > 0.99999f);

    const auto* y = link.on_client(far).get<world::Transform>();
    REQUIRE(y != nullptr);
    CHECK(std::abs(y->position.x - b.position.x) <= 0.0005);
    CHECK(std::abs(y->position.y - b.position.y) <= 0.0005);
    CHECK(std::abs(y->position.z - b.position.z) <= 0.0005);
}

TEST_CASE("Relevance limits entities to the area around the focus", "[replication]") {
    Link link;
    link.encoder.set_focus(1, math::DVec3{0.0, 0.0, 0.0});
    world::Entity close = link.server.create("Nah");
    close.set(at(10.0));
    world::Entity distant = link.server.create("Mittel");
    distant.set(at(100.0));
    world::Entity outside = link.server.create("Weit weg");
    outside.set(at(500.0));
    // Ohne Lage für alle relevant.
    world::Entity global = link.server.create("Wetter");
    global.set(Health{3});
    for (int i = 0; i < 3; ++i) {
        link.step();
    }
    CHECK(link.on_client(close).alive());
    CHECK(link.on_client(distant).alive());
    CHECK(link.on_client(global).alive());
    CHECK(link.mirror.local_id(outside.id()) == 0);

    // Hinein und hinaus.
    outside.set(at(20.0));
    close.set(at(1000.0));
    link.step();
    CHECK(link.on_client(outside).alive());
    CHECK(link.mirror.local_id(close.id()) == 0);

    // Ferne Entities (über 40 m) kommen nur jeden dritten Schnappschuss.
    u32 updates = 0;
    f64 last = 100.0;
    for (int i = 0; i < 6; ++i) {
        distant.set(at(100.0 + i + 1));
        link.step();
        const f64 seen = link.on_client(distant).get<world::Transform>()->position.x;
        if (seen != last) {
            ++updates;
            last = seen;
        }
    }
    CHECK(updates == 2);
}

TEST_CASE("Budget spreads a large world over several snapshots", "[replication]") {
    replication::EncoderSettings settings;
    settings.budget = 400;
    Link link{settings};
    std::vector<world::Entity> entities;
    for (int i = 0; i < 100; ++i) {
        entities.push_back(link.server.create(std::format("Kiste {}", i)));
        entities.back().set(at(i * 0.5));
    }
    // Ohne Bestätigung kommt immer dieselbe Auswahl, und die Welt wächst nicht.
    link.step(false);
    const usize first = link.mirror.entity_count();
    CHECK(first > 0);
    CHECK(first < 100);
    link.step(false);
    CHECK(link.mirror.entity_count() == first);

    int steps = 0;
    while (link.mirror.entity_count() < 100 && steps < 100) {
        CHECK(link.step() < settings.budget + 128);
        ++steps;
    }
    CHECK(link.mirror.entity_count() == 100);

    // Alle bewegen sich: Wer warten musste, kommt zuerst.
    for (int round = 0; round < 3; ++round) {
        for (world::Entity& entity : entities) {
            entity.set(at(entity.get<world::Transform>()->position.x + 1.0));
        }
        link.step();
    }
    CHECK(link.encoder.stats(1).waiting > 0);
    for (int i = 0; i < 100 && link.encoder.stats(1).waiting > 0; ++i) {
        link.step();
    }
    CHECK(link.encoder.stats(1).waiting == 0);
    CHECK(same_view(link.mirror.view(), link.encoder.captured()));
}

TEST_CASE("Entity references are translated to client ids", "[replication]") {
    replication::EncoderSettings settings;
    // Ein Eintrag pro Schnappschuss: Der Verweis kommt vor seinem Ziel an.
    settings.budget = 1;
    Link link{settings};
    world::Entity hunter = link.server.create("Jäger");
    world::Entity prey = link.server.create("Beute");
    hunter.set(Follow{prey.id()});
    REQUIRE(hunter.id() < prey.id());
    // Eigene Entities des Clients verschieben seine Kennungen gegenüber denen des Servers.
    (void)link.client.create("Versatz");
    (void)link.client.create("Versatz");

    link.step();
    CHECK(link.on_client(hunter).alive());
    CHECK(link.mirror.local_id(prey.id()) == 0);
    CHECK(link.on_client(hunter).get<Follow>()->target == 0);

    link.step();
    REQUIRE(link.on_client(prey).alive());
    CHECK(link.on_client(hunter).get<Follow>()->target == link.on_client(prey).id());
    CHECK(link.on_client(prey).id() != prey.id());
}

TEST_CASE("Parents follow the server hierarchy", "[replication]") {
    Link link;
    world::Entity child = link.server.create("Kind");
    world::Entity parent = link.server.create("Eltern");
    child.set(at(1.0));
    parent.set(at(5.0));
    child.set_parent(parent);
    link.step();
    CHECK(link.on_client(child).parent().id() == link.on_client(parent).id());

    child.set_parent(world::Entity{});
    link.step();
    CHECK_FALSE(link.on_client(child).parent().alive());
    CHECK(same_view(link.mirror.view(), link.encoder.captured()));
}

TEST_CASE("Mirror interpolates and briefly extrapolates transforms", "[replication]") {
    Link link;
    world::Entity box = link.server.create("Kiste");
    for (int tick = 1; tick <= 5; ++tick) {
        box.set(at(static_cast<f64>(tick)));
        link.step();
    }
    world::Entity mirrored = link.on_client(box);
    // Drei Ticks hinter dem neuesten Stand.
    link.mirror.interpolate(0.0, 3.0);
    CHECK(mirrored.get<world::Transform>()->position.x == Catch::Approx(2.0));
    link.mirror.interpolate(0.5 / replication::snapshot_rate, 3.0);
    CHECK(mirrored.get<world::Transform>()->position.x == Catch::Approx(2.5));
    CHECK(link.mirror.stats().interpolated == 1);
    CHECK(link.mirror.stats().extrapolated == 0);

    // Keine Schnappschüsse mehr: Die Kiste läuft kurz weiter, höchstens drei Ticks.
    link.mirror.interpolate(5.0 / replication::snapshot_rate, 3.0);
    CHECK(link.mirror.stats().extrapolated == 1);
    CHECK(mirrored.get<world::Transform>()->position.x == Catch::Approx(7.25));
    link.mirror.interpolate(5.0 / replication::snapshot_rate, 3.0);
    CHECK(mirrored.get<world::Transform>()->position.x == Catch::Approx(8.0));
}

TEST_CASE("Mirror bridges sparse and lost updates of moving entities", "[replication]") {
    // Fern (über 40 m) kommt nur jeder dritte Stand; gehen dazu Schnappschüsse verloren, darf die
    // Entity nicht stehen bleiben und dann springen (R5.3, Abnahme).
    Link link;
    link.encoder.set_focus(1, math::DVec3{0.0, 0.0, 0.0});
    world::Entity distant = link.server.create("Fern");
    // Von 60 m bis 140 m: immer über 40 m und innerhalb der Relevanz (150 m).
    distant.set(at(60.0));
    link.step();
    link.step();
    const world::Entity mirrored = link.on_client(distant);
    REQUIRE(mirrored.alive());
    std::vector<f64> seen;
    for (int i = 1; i <= 80; ++i) {
        distant.set(at(60.0 + i));
        if (i >= 45 && i <= 47) {
            link.lose();
        } else {
            link.step();
        }
        link.mirror.interpolate(1.0 / replication::snapshot_rate);
        if (i >= 30) {
            seen.push_back(mirrored.get<world::Transform>()->position.x);
        }
    }
    for (usize i = 1; i < seen.size(); ++i) {
        INFO("Bild " << i << ": " << seen[i - 1] << " -> " << seen[i]);
        CHECK(seen[i] >= seen[i - 1]);
        CHECK(seen[i] - seen[i - 1] < 2.0);
    }
    CHECK(seen.back() > seen.front() + 40.0);
}

TEST_CASE("Welcome detects a different schema", "[replication]") {
    const auto types = make_types();
    const replication::Schema schema{types};
    auto welcome = replication::encode_welcome(schema);
    REQUIRE(welcome[0] == static_cast<std::byte>(replication::MessageKind::Welcome));
    const auto body = std::span{welcome}.subspan(1);
    CHECK(replication::check_welcome(body, schema));

    const auto other_types = make_types(true);
    const replication::Schema other{other_types};
    CHECK_FALSE(replication::check_welcome(body, other));
    CHECK_FALSE(replication::check_welcome(body.first(3), schema));

    const auto ack = replication::encode_ack(42);
    REQUIRE(ack[0] == static_cast<std::byte>(replication::MessageKind::Ack));
    CHECK(replication::decode_ack(std::span{ack}.subspan(1)) == 42u);
    CHECK_FALSE(replication::decode_ack(std::span{ack}.subspan(1, 2)).has_value());
}

namespace {

// Server und Client über localhost; ein Schnappschuss alle 33 ms wie im Spiel.
struct Session {
    Session(const replication::Schema& server_schema, const replication::Schema& client_schema,
            world::World& server_world, world::World& client_world) {
        auto st = net::Transport::create();
        auto ct = net::Transport::create();
        REQUIRE(st);
        REQUIRE(ct);
        server_transport = std::move(*st);
        client_transport = std::move(*ct);
        const auto port = server_transport->listen(0);
        REQUIRE(port);
        server =
            std::make_unique<replication::Server>(*server_transport, server_world, server_schema);
        client =
            std::make_unique<replication::Client>(*client_transport, client_world, client_schema);
        REQUIRE(client->connect(std::format("127.0.0.1:{}", *port)));
    }

    // Pumpt, bis done() gilt oder die Zeit abläuft; before() läuft vor jedem Schnappschuss.
    template <typename Done, typename Before>
    bool pump(Done&& done, Before&& before,
              std::chrono::seconds timeout = std::chrono::seconds{5}) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            server->poll(server_received);
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_snapshot) {
                before();
                server->send_snapshot();
                next_snapshot = now + std::chrono::milliseconds{33};
            }
            if (!client->poll(client_received)) {
                return false;
            }
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    }
    template <typename Done>
    bool pump(Done&& done) {
        return pump(std::forward<Done>(done), [] {});
    }

    std::unique_ptr<net::Transport> server_transport;
    std::unique_ptr<net::Transport> client_transport;
    std::unique_ptr<replication::Server> server;
    std::unique_ptr<replication::Client> client;
    std::vector<replication::Received> server_received;
    std::vector<replication::Received> client_received;
    std::chrono::steady_clock::time_point next_snapshot{};
};

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

} // namespace

TEST_CASE("Server replicates a component to a client over localhost", "[replication][net]") {
    const auto types = make_types();
    const replication::Schema schema{types};
    world::World server_world{types};
    world::World client_world{types};
    Session session{schema, schema, server_world, client_world};
    world::Entity player = server_world.create("Spieler");
    player.set(Position{0.0f, 0.0f});

    REQUIRE(
        session.pump([&] { return session.client->state() == replication::ClientState::Ready; }));
    CHECK(session.server->clients().size() == 1);

    // Der Client schickt eine Eingabe, der Server bewegt den Spieler, der Client sieht es.
    const std::array input{std::byte{1}};
    REQUIRE(session.client->send(input, net::Delivery::Reliable));
    REQUIRE(session.pump([&] { return !session.server_received.empty(); }));
    CHECK(session.server_received.front().data == std::vector<std::byte>{std::byte{1}});
    player.set(Position{7.0f, 8.0f});

    REQUIRE(session.pump([&] {
        const u64 local = session.client->mirror().local_id(player.id());
        if (local == 0) {
            return false;
        }
        const auto* position = client_world.entity(local).get<Position>();
        return position != nullptr && position->x == 7.0f && position->y == 8.0f;
    }));
    CHECK(session.client->snapshots_applied() > 0);
    // Der Client bestätigt: Der Server schickt Deltas.
    REQUIRE(session.pump(
        [&] { return session.server->stats(session.server->clients().front()).acked_tick > 0; }));

    const std::array reply{std::byte{2}, std::byte{3}};
    REQUIRE(
        session.server->send(session.server->clients().front(), reply, net::Delivery::Reliable));
    REQUIRE(session.pump([&] { return !session.client_received.empty(); }));
    CHECK(session.client_received.front().data ==
          std::vector<std::byte>{std::byte{2}, std::byte{3}});

    session.client->disconnect();
    CHECK(session.client->state() == replication::ClientState::Disconnected);
    CHECK(client_world.entity_count() == 0);
    session.server->poll(session.server_received);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!session.server->clients().empty() && std::chrono::steady_clock::now() < deadline) {
        session.server->poll(session.server_received);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(session.server->clients().empty());
}

TEST_CASE("Client heals to the server world despite lag and loss", "[replication][net]") {
    // Beide Seiten im selben Prozess: je 10 % beim Senden und Empfangen sind zusammen 19 %
    // Verlust pro Richtung, die Verzögerung zählt doppelt (100 ms hin, 100 ms zurück).
    const SimulatedNetwork network{{.lag_ms = 100, .loss_percent = 10.0f}};
    const auto types = make_types();
    const replication::Schema schema{types};
    world::World server_world{types};
    world::World client_world{types};
    std::vector<world::Entity> boxes;
    for (int i = 0; i < 40; ++i) {
        boxes.push_back(server_world.create(std::format("Kiste {}", i)));
        boxes.back().set(at(i * 2.0)).set(Health{i});
    }
    Session session{schema, schema, server_world, client_world};
    REQUIRE(session.pump([&] { return session.client->state() == replication::ClientState::Ready; },
                         [] {}, std::chrono::seconds{10}));

    // Zwei Sekunden lang ändert sich alles, Entities kommen und gehen.
    int frame = 0;
    const auto move = [&] {
        world::update_transforms(server_world);
        ++frame;
        for (usize i = 0; i < boxes.size(); ++i) {
            boxes[i].set(at(static_cast<f64>(i) * 2.0 + frame * 0.1));
        }
        if (frame % 10 == 0) {
            boxes.front().destroy();
            boxes.erase(boxes.begin());
            boxes.push_back(server_world.create(std::format("Neu {}", frame)));
            boxes.back().set(at(0.0)).set(Health{frame});
        }
        world::update_transforms(server_world);
    };
    session.pump([] { return false; }, move, std::chrono::seconds{2});

    // Danach steht alles still; der Client muss genau den Stand des Servers erreichen.
    replication::SnapshotEncoder reference{server_world, schema};
    reference.capture(1);
    const replication::View expected = reference.captured();
    REQUIRE(session.pump([&] { return same_view(session.client->mirror().view(), expected); },
                         [] {}, std::chrono::seconds{10}));
    CHECK(session.client->mirror().entity_count() == boxes.size());
    for (const world::Entity& box : boxes) {
        const world::Entity mirrored =
            client_world.entity(session.client->mirror().local_id(box.id()));
        REQUIRE(mirrored.alive());
        CHECK(mirrored.get<Health>()->value == box.get<Health>()->value);
        CHECK(std::abs(mirrored.get<world::Transform>()->position.x -
                       box.get<world::Transform>()->position.x) <= 0.0005);
    }
    CHECK(net::Transport::simulated().lag_ms == 100);
}

TEST_CASE("Client disconnects when the server replicates other components", "[replication][net]") {
    const auto types = make_types();
    const auto other_types = make_types(true);
    const replication::Schema schema{types};
    const replication::Schema other{other_types};
    world::World server_world{types};
    world::World client_world{other_types};
    Session session{schema, other, server_world, client_world};

    std::vector<replication::Received> received;
    bool failed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!failed && std::chrono::steady_clock::now() < deadline) {
        session.server->poll(received);
        failed = !session.client->poll(received);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(failed);
    CHECK(session.client->state() == replication::ClientState::Disconnected);
}

namespace {

replication::Command make_command(u32 number, const input::ActionMap& map) {
    replication::Command command{
        .number = number, .yaw = 0.5f * static_cast<f32>(number), .pitch = -0.25f, .actions = {}};
    command.actions.resize(map.actions().size());
    command.actions[0] = {.x = 0.25f, .y = -1.0f, .down = true};
    command.actions[4] = {.x = 1.0f, .down = true, .pressed = true};
    command.actions[5] = {.released = true};
    return command;
}

} // namespace

TEST_CASE("Commands survive encoding and reject a different map", "[replication]") {
    const input::ActionMap map = input::ActionMap::standard();
    const std::vector<replication::Command> commands{make_command(7, map), make_command(8, map),
                                                     make_command(9, map)};
    const std::vector<std::byte> message = replication::encode_commands(map.hash(), commands);
    CHECK(replication::game_message(message) == replication::GameMessage::Commands);
    // Werte gehen nur mit, wenn sie nicht 0 sind: zwei Werte zu je 8 Bytes pro Befehl.
    CHECK(message.size() == 8 + 3 * (12 + map.actions().size() + 2 * 8));
    const auto decoded = replication::decode_commands(message, map.hash());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == commands.size());
    for (usize c = 0; c < commands.size(); ++c) {
        const replication::Command& a = commands[c];
        const replication::Command& b = (*decoded)[c];
        CHECK(a.number == b.number);
        CHECK(a.yaw == b.yaw);
        CHECK(a.pitch == b.pitch);
        REQUIRE(a.actions.size() == b.actions.size());
        for (usize i = 0; i < a.actions.size(); ++i) {
            CHECK(a.actions[i].x == b.actions[i].x);
            CHECK(a.actions[i].y == b.actions[i].y);
            CHECK(a.actions[i].down == b.actions[i].down);
            CHECK(a.actions[i].pressed == b.actions[i].pressed);
            CHECK(a.actions[i].released == b.actions[i].released);
        }
    }

    // Andere Belegung (Aktion hinzugefügt): abgelehnt.
    input::ActionMap other = map;
    REQUIRE(other.add({.name = "tanzen"}).has_value());
    CHECK(other.hash() != map.hash());
    CHECK_FALSE(replication::decode_commands(message, other.hash()).has_value());
    // Abgeschnitten oder mit überzähligem Byte: abgelehnt.
    const std::span<const std::byte> cut{message.data(), message.size() - 1};
    CHECK_FALSE(replication::decode_commands(cut, map.hash()).has_value());
    std::vector<std::byte> longer = message;
    longer.push_back(std::byte{0});
    CHECK_FALSE(replication::decode_commands(longer, map.hash()).has_value());
    // Nummern rückwärts: abgelehnt.
    const std::vector<replication::Command> backwards{make_command(9, map), make_command(8, map)};
    CHECK_FALSE(replication::decode_commands(replication::encode_commands(map.hash(), backwards),
                                             map.hash())
                    .has_value());
    // Nicht endlicher Blick: abgelehnt.
    std::vector<replication::Command> broken{make_command(1, map)};
    broken[0].yaw = std::nanf("");
    CHECK_FALSE(
        replication::decode_commands(replication::encode_commands(map.hash(), broken), map.hash())
            .has_value());
}

TEST_CASE("Server limits commands to the action map", "[replication]") {
    const input::ActionMap map = input::ActionMap::standard();
    const auto move = map.find("move");
    const auto jump = map.find("jump");
    REQUIRE(move);
    REQUIRE(jump);
    replication::Command command = make_command(1, map);
    command.actions[*move] = {.x = 3.0f, .y = 4.0f, .down = true};
    command.actions[*jump] = {.x = 7.0f, .down = true};
    command.pitch = 3.0f;
    CHECK(replication::limit_command(command, map, 1.55f) == 3);
    CHECK(command.actions[*move].x == Catch::Approx(0.6f));
    CHECK(command.actions[*move].y == Catch::Approx(0.8f));
    CHECK(command.actions[*jump].x == 1.0f);
    CHECK(command.pitch == 1.55f);
    // Diagonal mit WASD bleibt erlaubt.
    replication::Command fine = make_command(2, map);
    fine.actions[*move] = {.x = 0.70710677f, .y = 0.70710677f, .down = true};
    fine.actions[4] = {};
    CHECK(replication::limit_command(fine, map, 1.55f) == 0);
}

TEST_CASE("Command queue keeps order, repeats gaps and drops late commands", "[replication]") {
    const input::ActionMap map = input::ActionMap::standard();
    replication::CommandQueue queue{2, 4};
    queue.push(make_command(11, map));
    // Erst ab zwei wartenden Befehlen.
    CHECK_FALSE(queue.next().has_value());
    queue.push(make_command(10, map));
    queue.push(make_command(10, map));
    CHECK(queue.stats().dropped == 1);
    auto first = queue.next();
    REQUIRE(first);
    CHECK(first->number == 10);
    auto second = queue.next();
    REQUIRE(second);
    CHECK(second->number == 11);
    // 12 fehlt: 11 gilt noch einmal unter der Nummer 12, ohne Flanken.
    auto repeated = queue.next();
    REQUIRE(repeated);
    CHECK(repeated->number == 12);
    CHECK(repeated->yaw == second->yaw);
    CHECK(repeated->actions[4].down);
    CHECK_FALSE(repeated->actions[4].pressed);
    CHECK_FALSE(repeated->actions[5].released);
    CHECK(queue.stats().repeated == 1);
    // 12 kommt zu spät und wird verworfen, 13 gilt.
    queue.push(make_command(12, map));
    queue.push(make_command(13, map));
    CHECK(queue.stats().dropped == 2);
    auto next = queue.next();
    REQUIRE(next);
    CHECK(next->number == 13);
    CHECK(queue.last() == 13);
    // Überlauf: die ältesten gehen.
    for (u32 n = 14; n < 20; ++n) {
        queue.push(make_command(n, map));
    }
    CHECK(queue.size() == 4);
    CHECK(queue.stats().overflow == 2);
    auto after = queue.next();
    REQUIRE(after);
    CHECK(after->number == 16);
}

TEST_CASE("Command queue stops repeating input after a long gap", "[replication]") {
    const input::ActionMap map = input::ActionMap::standard();
    replication::CommandQueue queue{1, 8, 3};
    queue.push(make_command(1, map));
    auto first = queue.next();
    REQUIRE(first);
    REQUIRE(first->actions[4].down);
    // Drei Mal gilt der letzte weiter unter den fehlenden Nummern, danach ohne Eingabe und ohne
    // neue Nummer, aber mit Blick.
    for (u32 i = 2; i <= 6; ++i) {
        auto command = queue.next();
        REQUIRE(command);
        CHECK(command->yaw == first->yaw);
        if (i <= 4) {
            CHECK(command->number == i);
            CHECK(command->actions[4].down);
        } else {
            CHECK(command->number == 4);
            CHECK_FALSE(command->actions[4].down);
            CHECK(command->actions[4].x == 0.0f);
        }
    }
    CHECK(queue.stats().repeated == 5);
    CHECK(queue.stats().idle == 2);
    CHECK(queue.last() == 4);
    // Nach dem Stau gilt der nächste, der kommt, obwohl 5 und 6 fehlen; die Zählung beginnt neu.
    queue.push(make_command(7, map));
    auto back = queue.next();
    REQUIRE(back);
    CHECK(back->number == 7);
    CHECK(back->actions[4].down);
    auto again = queue.next();
    REQUIRE(again);
    CHECK(again->number == 8);
    CHECK(again->actions[4].down);
    CHECK(queue.stats().dropped == 0);
}

TEST_CASE("Owner state carries the exact figure state", "[replication]") {
    const replication::OwnerState state{.command = 1234,
                                        .queued = 3,
                                        .position = {1234567.125, -3.0000001, 0.1},
                                        .velocity = {1.5f, -9.81f, 0.25f},
                                        .ground = 1,
                                        .crouched = true,
                                        .touching = true};
    const auto message = replication::encode_owner(state);
    CHECK(replication::game_message(message) == replication::GameMessage::Owner);
    const auto decoded = replication::decode_owner(message);
    REQUIRE(decoded.has_value());
    CHECK(decoded->command == 1234);
    CHECK(decoded->queued == 3);
    CHECK(decoded->position.x == state.position.x);
    CHECK(decoded->position.y == state.position.y);
    CHECK(decoded->position.z == state.position.z);
    CHECK(decoded->velocity.y == state.velocity.y);
    CHECK(decoded->ground == 1);
    CHECK(decoded->crouched);
    CHECK(decoded->touching);
    CHECK_FALSE(
        replication::decode_owner(std::span{message.data(), message.size() - 1}).has_value());
    auto unknown = message;
    unknown.back() = std::byte{8};
    CHECK_FALSE(replication::decode_owner(unknown).has_value());
}

namespace {

// Eine Figur, die nur in x läuft: Lage += Geschwindigkeit aus dem ersten Wert des Befehls.
struct LineWorld {
    replication::PredictedState state;

    replication::PredictedState step(const replication::Command& command) {
        state.velocity.x = command.actions.empty() ? 0.0f : command.actions[0].x;
        state.position.x += static_cast<f64>(state.velocity.x) / 60.0;
        return state;
    }
};

replication::Command line_command(u32 number, f32 speed) {
    return replication::Command{.number = number,
                                .yaw = 0.0f,
                                .pitch = 0.0f,
                                .actions = {input::ActionValue{.x = speed, .down = true}}};
}

} // namespace

TEST_CASE("Predictor keeps matching predictions and replays after a mismatch", "[replication]") {
    replication::Predictor predictor;
    LineWorld client;
    LineWorld server;
    std::vector<replication::PredictedState> server_states;
    for (u32 n = 1; n <= 10; ++n) {
        const auto command = line_command(n, 3.0f);
        CHECK(predictor.next_number() == n);
        predictor.record(command, client.step(command));
        server_states.push_back(server.step(command));
    }
    const auto set = [&](const replication::PredictedState& state) { client.state = state; };
    const auto step = [&](const replication::Command& command) { return client.step(command); };

    // Gleicher Stand: keine Korrektur, bestätigte Befehle fallen weg.
    CHECK_FALSE(predictor.reconcile(4, server_states[3], set, step));
    CHECK(predictor.stats().pending == 6);
    CHECK(predictor.stats().acknowledged == 4);
    CHECK(predictor.recent(3).back().number == 10);
    // Doppelt oder älter: nichts.
    CHECK_FALSE(predictor.reconcile(3, server_states[2], set, step));

    // Der Server wurde nach Befehl 6 um 1 m geschoben: Korrektur und Neurechnen von 7 bis 10.
    replication::PredictedState pushed = server_states[5];
    pushed.position.x += 1.0;
    CHECK(predictor.reconcile(6, pushed, set, step));
    CHECK(predictor.stats().corrections == 1);
    CHECK(predictor.stats().replayed == 4);
    CHECK(predictor.stats().last_correction == Catch::Approx(1.0));
    const auto latest = predictor.latest();
    REQUIRE(latest);
    CHECK(latest->position.x == Catch::Approx(server_states[9].position.x + 1.0));
    CHECK(client.state.position.x == Catch::Approx(server_states[9].position.x + 1.0));

    // Kleine Abweichungen unter der Toleranz zählen nicht.
    replication::PredictedState close = *latest;
    close.position.x += 0.004;
    CHECK_FALSE(predictor.reconcile(10, close, set, step));
    CHECK(predictor.stats().corrections == 1);
}

TEST_CASE("Possess names the figure of a client", "[replication]") {
    const auto message = replication::encode_possess(0x0123456789abcdefULL);
    CHECK(replication::game_message(message) == replication::GameMessage::Possess);
    const auto entity = replication::decode_possess(message);
    REQUIRE(entity.has_value());
    CHECK(*entity == 0x0123456789abcdefULL);
    CHECK_FALSE(replication::decode_possess(std::span{message.data(), 4}).has_value());
    const std::array<std::byte, 1> unknown{std::byte{42}};
    CHECK_FALSE(replication::game_message(unknown).has_value());
}

TEST_CASE("Events and sounds survive encoding and reject broken messages", "[replication]") {
    const replication::NetEvent event{.tick = 77, .entity = 0x1122334455667788ULL, .value = 2.5f,
                                      .name = "Licht"};
    const auto message = replication::encode_event(event);
    CHECK(replication::game_message(message) == replication::GameMessage::Event);
    const auto decoded = replication::decode_event(message);
    REQUIRE(decoded.has_value());
    CHECK(decoded->tick == 77);
    CHECK(decoded->entity == event.entity);
    CHECK(decoded->value == 2.5f);
    CHECK(decoded->name == "Licht");
    CHECK_FALSE(replication::decode_event(std::span{message.data(), message.size() - 1}));
    // Lange Namen werden gekürzt.
    const auto long_name = replication::encode_event({.name = std::string(200, 'x')});
    CHECK(replication::decode_event(long_name)->name.size() == replication::max_event_name);

    replication::NetSound sound{.tick = 9, .sound = 42, .position = math::DVec3{1.0, -2.0, 3.5},
                                .bus = 2, .volume = 0.5f, .pitch = 1.25f};
    const auto positioned = replication::encode_sound(sound);
    CHECK(replication::game_message(positioned) == replication::GameMessage::Sound);
    const auto back = replication::decode_sound(positioned);
    REQUIRE(back.has_value());
    CHECK(back->tick == 9);
    CHECK(back->sound == 42);
    REQUIRE(back->position.has_value());
    CHECK(back->position->y == -2.0);
    CHECK(back->bus == 2);
    CHECK(back->volume == 0.5f);
    CHECK(back->pitch == 1.25f);
    sound.position.reset();
    const auto plain = replication::encode_sound(sound);
    CHECK(plain.size() + 24 == positioned.size());
    CHECK_FALSE(replication::decode_sound(plain)->position.has_value());
    CHECK_FALSE(replication::decode_sound(std::span{positioned.data(), plain.size()}));
    // Nicht endliche Werte gelten als kaputt.
    sound.volume = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(replication::decode_sound(replication::encode_sound(sound)));
}

TEST_CASE("Encoder answers relevance like the snapshot", "[replication]") {
    Link link;
    link.encoder.set_focus(1, math::DVec3{0.0, 0.0, 0.0});
    world::Entity close = link.server.create("Nah");
    close.set(at(10.0));
    world::Entity outside = link.server.create("Weit weg");
    outside.set(at(500.0));
    world::Entity global = link.server.create("Wetter");
    global.set(Health{3});
    link.step();
    CHECK(link.encoder.relevant(1, close.id()));
    CHECK_FALSE(link.encoder.relevant(1, outside.id()));
    CHECK(link.encoder.relevant(1, global.id()));
    // Unbekannte Clients und tote Entities sind nie relevant.
    CHECK_FALSE(link.encoder.relevant(2, close.id()));
    const u64 gone = close.id();
    close.destroy();
    CHECK_FALSE(link.encoder.relevant(1, gone));
    CHECK(link.encoder.near(1, math::DVec3{30.0, 0.0, 0.0}, 50.0));
    CHECK_FALSE(link.encoder.near(1, math::DVec3{60.0, 0.0, 0.0}, 50.0));
    // Gezeichnete Zeit nach interpolate().
    link.mirror.interpolate(0.0, 3.0);
    CHECK(link.mirror.render_tick() == Catch::Approx(link.mirror.last_tick() - 3.0));
}
