#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.reflect;
import ngine.replication;
import ngine.voxel;
import ngine.world;

// Terrain übers Netz (R5.4, ADR 0054): Nachricht, Sender des Servers und Empfänger des Clients mit
// vorhergesagten Pinseln, ohne Netz.

using namespace ngine;

namespace {

constexpr voxel::GeneratorDesc test_world{.seed = 7};

// Krater im Gitter des Terrains (Voxel), ohne geladene Chunks.
voxel::TerrainEdit crater(voxel::TerrainDeltas& deltas, f32 x, f32 z, f32 radius) {
    voxel::ChunkStore store;
    const auto ground = static_cast<f32>(
        deltas.generator().height(static_cast<i64>(x), static_cast<i64>(z)));
    return deltas.apply(store,
                        {.mode = voxel::BrushMode::Remove, .center = {x, ground, z}, .radius = radius});
}

std::vector<replication::TerrainMessage> decode_all(std::span<const std::vector<std::byte>> data) {
    std::vector<replication::TerrainMessage> messages;
    for (const auto& bytes : data) {
        auto message = replication::decode_terrain(bytes);
        INFO((message ? std::string{} : message.error()));
        REQUIRE(message);
        messages.push_back(std::move(*message));
    }
    return messages;
}

bool same(const std::shared_ptr<const voxel::ChunkDelta>& a,
          const std::shared_ptr<const voxel::ChunkDelta>& b) {
    if (!a || !b) {
        return !a == !b;
    }
    return *a == *b;
}

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    replication::register_types(types);
    return types;
}

// Welt eines Clients mit einem Terrain, das der Server unter server_id kennt.
struct ClientWorld {
    explicit ClientWorld(u64 server_id) {
        terrain = world.create("Terrain");
        terrain.set(world::Terrain{.size_x = 4,
                                   .size_y = 2,
                                   .size_z = 4,
                                   .shape = world::TerrainShape::Flat,
                                   .seed = 7,
                                   .ground = 10.0f});
        terrain.set(replication::Replicated{.server_id = server_id});
        this->server_id = server_id;
    }
    void build() {
        (void)world::update_terrains(world, jobs, {.wait = true});
    }
    [[nodiscard]] u64 local(u64 id) const {
        return id == server_id ? terrain.id() : 0;
    }
    usize apply(replication::TerrainReceiver& receiver) {
        return receiver.apply(world, [this](u64 id) { return local(id); });
    }
    [[nodiscard]] std::shared_ptr<const voxel::ChunkDelta> delta(voxel::ChunkCoord c) const {
        const auto map = world::terrain_deltas(terrain);
        if (!map) {
            return nullptr;
        }
        const auto it = map->find(c);
        return it != map->end() ? it->second : nullptr;
    }

    reflect::TypeRegistry types = make_types();
    world::World world{types};
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Entity terrain;
    u64 server_id = 0;
};

// Ein Pinsel mitten auf das flache Terrain (Boden in 10 m, also 20 Voxel über der Unterkante).
voxel::Brush dig_brush(f32 x, f32 z) {
    return {.mode = voxel::BrushMode::Remove, .center = {x, 20.0f, z}, .radius = 3.0f};
}

} // namespace

TEST_CASE("Terrain messages survive encoding and reject broken data", "[replication]") {
    voxel::TerrainDeltas deltas{test_world};
    REQUIRE_FALSE(crater(deltas, -40.0f, 20.0f, 6.0f).empty());
    replication::TerrainMessage message{.terrain = 42, .acknowledged = 7, .complete = true,
                                        .chunks = {}};
    for (const auto& [coord, delta] : *deltas.snapshot()) {
        message.chunks.push_back({coord, delta});
    }
    // Rückkehr zur Grundwelt und große Koordinaten mit Vorzeichen.
    message.chunks.push_back({{-70000, 5, 123456, 0}, nullptr});
    const std::vector<std::byte> bytes = replication::encode_terrain(message);
    CHECK(replication::game_message(bytes) == replication::GameMessage::Terrain);

    const auto decoded = replication::decode_terrain(bytes);
    INFO((decoded ? std::string{} : decoded.error()));
    REQUIRE(decoded);
    CHECK(decoded->terrain == 42);
    CHECK(decoded->acknowledged == 7);
    CHECK(decoded->complete);
    REQUIRE(decoded->chunks.size() == message.chunks.size());
    for (usize i = 0; i < message.chunks.size(); ++i) {
        CHECK(decoded->chunks[i].coord == message.chunks[i].coord);
        CHECK(same(decoded->chunks[i].delta, message.chunks[i].delta));
    }

    // Abgeschnitten, überzählig, unbekannte Flags, kaputtes Delta.
    CHECK_FALSE(replication::decode_terrain(std::span{bytes}.first(bytes.size() - 1)));
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(replication::decode_terrain(longer));
    std::vector<std::byte> flags = bytes;
    flags[1 + 8 + 4] = std::byte{0x80};
    CHECK_FALSE(replication::decode_terrain(flags));
    replication::TerrainMessage broken{.terrain = 1, .acknowledged = 0, .complete = false,
                                       .chunks = {}};
    std::vector<std::byte> bad = replication::encode_terrain(broken);
    bad.back() = std::byte{1}; // ein Chunk
    for (const std::byte b : {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{3},
                              std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}}) {
        bad.push_back(b);
    }
    CHECK_FALSE(replication::decode_terrain(bad));
    CHECK_FALSE(replication::decode_terrain({}));
}

TEST_CASE("Terrain sender sends changed chunks once, near ones first", "[replication]") {
    voxel::TerrainDeltas deltas{test_world};
    // Zwei Krater: einer am Ursprung, einer gut 500 m entfernt.
    REQUIRE_FALSE(crater(deltas, 4.0f, 4.0f, 6.0f).empty());
    REQUIRE_FALSE(crater(deltas, 1000.0f, 1000.0f, 12.0f).empty());
    const usize total = deltas.size();

    replication::TerrainSender sender{{.relevance = 150.0, .budget = 64}};
    sender.update_terrain(5, deltas.snapshot(), {});
    sender.add_client(1);
    sender.set_focus(1, math::DVec3{0.0, 0.0, 0.0});

    // Erst alles in der Relevanz (ohne Budget) und „Umgebung vollständig“, ferne Chunks gedrosselt.
    auto first = decode_all(sender.encode(1, 3));
    REQUIRE(first.size() == 1);
    CHECK(first[0].terrain == 5);
    CHECK(first[0].acknowledged == 3);
    CHECK(first[0].complete);
    usize received = first[0].chunks.size();
    for (const replication::TerrainChunk& chunk : first[0].chunks) {
        CHECK(same(chunk.delta, deltas.find(chunk.coord)));
    }
    CHECK(received < total);
    CHECK(sender.stats(1).complete);
    CHECK(sender.stats(1).pending > 0);
    // Pro Aufruf mindestens ein ferner Chunk, bis alles da ist.
    for (int i = 0; i < 100 && sender.stats(1).pending > 0; ++i) {
        for (const auto& message : decode_all(sender.encode(1, 3))) {
            CHECK(message.chunks.size() == 1);
            received += message.chunks.size();
        }
    }
    CHECK(received == total);
    CHECK(sender.stats(1).pending == 0);

    // Nichts Neues: keine Nachricht, außer der Client hat gegraben (nur die Bestätigung).
    CHECK(sender.encode(1, 4).empty());
    auto ack = decode_all(sender.encode(1, 5, true));
    REQUIRE(ack.size() == 1);
    CHECK(ack[0].acknowledged == 5);
    CHECK(ack[0].chunks.empty());
    CHECK(ack[0].complete);

    // Eine neue Momentaufnahme schickt nur die geänderten Chunks.
    const voxel::TerrainEdit edit = crater(deltas, 12.0f, 4.0f, 3.0f);
    REQUIRE_FALSE(edit.empty());
    sender.update_terrain(5, deltas.snapshot(), {});
    auto change = decode_all(sender.encode(1, 6));
    REQUIRE(change.size() == 1);
    CHECK(change[0].chunks.size() == edit.changes.size());

    // Ein später beitretender Client bekommt alles.
    sender.add_client(2);
    sender.set_focus(2, math::DVec3{1000.0, 0.0, 1000.0});
    usize late = 0;
    for (int i = 0; i < 100; ++i) {
        for (const auto& message : decode_all(sender.encode(2, 0))) {
            late += message.chunks.size();
        }
        if (sender.stats(2).pending == 0) {
            break;
        }
    }
    CHECK(late == deltas.size());
    CHECK(sender.stats(2).complete);
}

TEST_CASE("Terrain sender reports removed deltas and forgotten terrains", "[replication]") {
    voxel::TerrainDeltas deltas{test_world};
    const voxel::TerrainEdit edit = crater(deltas, 4.0f, 4.0f, 6.0f);
    REQUIRE_FALSE(edit.empty());
    replication::TerrainSender sender;
    sender.update_terrain(9, deltas.snapshot(), {});
    sender.add_client(1);
    sender.set_focus(1, math::DVec3{0.0, 0.0, 0.0});
    REQUIRE(decode_all(sender.encode(1, 0)).size() == 1);

    // Undo: die Chunks kehren zur Grundwelt zurück, der Client bekommt leere Deltas.
    voxel::ChunkStore store;
    (void)deltas.restore(store, edit.changes, false);
    sender.update_terrain(9, deltas.snapshot(), {});
    auto back = decode_all(sender.encode(1, 0));
    REQUIRE(back.size() == 1);
    CHECK(back[0].chunks.size() == edit.changes.size());
    for (const replication::TerrainChunk& chunk : back[0].chunks) {
        CHECK(chunk.delta == nullptr);
    }

    // Ein Terrain ohne Deltas meldet trotzdem einmal „Umgebung vollständig“.
    replication::TerrainSender empty;
    empty.update_terrain(3, nullptr, {});
    empty.add_client(1);
    CHECK(empty.encode(1, 0).empty()); // ohne Figur weiß der Server nicht, was nah ist
    empty.set_focus(1, math::DVec3{0.0, 0.0, 0.0});
    auto complete = decode_all(empty.encode(1, 0));
    REQUIRE(complete.size() == 1);
    CHECK(complete[0].complete);
    CHECK(empty.encode(1, 0).empty());

    sender.retain_terrains({});
    CHECK(sender.encode(1, 0, true).empty());
}

TEST_CASE("Terrain receiver waits for its terrain and applies server deltas", "[replication]") {
    ClientWorld client{77};
    replication::TerrainReceiver receiver;

    // Deltas des Servers für dasselbe Terrain.
    ClientWorld server{77};
    server.build();
    const voxel::TerrainEdit edit = world::edit_terrain(server.terrain, dig_brush(32.0f, 32.0f));
    REQUIRE_FALSE(edit.empty());
    replication::TerrainMessage message{.terrain = 77, .acknowledged = 0, .complete = true,
                                        .chunks = {}};
    for (const voxel::DeltaChange& change : edit.changes) {
        message.chunks.push_back({change.coord, change.after});
    }
    REQUIRE(receiver.receive(replication::encode_terrain(message)));
    // Ein Terrain, das der Client nicht kennt, hält nichts auf.
    receiver.receive(replication::TerrainMessage{.terrain = 5, .acknowledged = 0,
                                                 .complete = false, .chunks = {}});

    // Ohne Laufzeitdaten wartet die Nachricht.
    CHECK(client.apply(receiver) == 0);
    CHECK(receiver.stats().waiting == 2);
    CHECK_FALSE(receiver.complete());
    client.build();
    CHECK(client.apply(receiver) == 1);
    CHECK(receiver.stats().waiting == 1);
    CHECK(receiver.complete());
    for (const voxel::DeltaChange& change : edit.changes) {
        CHECK(same(client.delta(change.coord), change.after));
        CHECK(same(receiver.confirmed(77, change.coord), change.after));
    }
    CHECK(world::terrain_deltas(client.terrain)->size() ==
          world::terrain_deltas(server.terrain)->size());
}

TEST_CASE("Predicted brushes stay until confirmed and roll back otherwise", "[replication]") {
    ClientWorld client{77};
    client.build();
    ClientWorld server{77};
    server.build();
    replication::TerrainReceiver receiver;

    // Befehl 10 gräbt beim Client sofort.
    receiver.predict(client.terrain, 10, dig_brush(32.0f, 32.0f));
    REQUIRE(world::terrain_deltas(client.terrain)->size() > 0);
    CHECK(receiver.stats().predicted == 1);

    // Der Server bestätigt Befehl 9: Die Grube bleibt.
    REQUIRE(receiver.receive(replication::encode_terrain(
        {.terrain = 77, .acknowledged = 9, .complete = true, .chunks = {}})));
    CHECK(client.apply(receiver) == 1);
    CHECK(receiver.stats().predicted == 1);
    CHECK(world::terrain_deltas(client.terrain)->size() > 0);

    // Der Server gräbt im selben Befehl an derselben Stelle und bestätigt ihn: gleiches Ergebnis.
    const voxel::TerrainEdit edit = world::edit_terrain(server.terrain, dig_brush(32.0f, 32.0f));
    replication::TerrainMessage confirm{.terrain = 77, .acknowledged = 10, .complete = true,
                                        .chunks = {}};
    for (const voxel::DeltaChange& change : edit.changes) {
        confirm.chunks.push_back({change.coord, change.after});
    }
    REQUIRE(receiver.receive(replication::encode_terrain(confirm)));
    CHECK(client.apply(receiver) == 1);
    CHECK(receiver.stats().predicted == 0);
    CHECK(receiver.stats().confirmed == 1);
    CHECK(receiver.stats().mismatched == 0);
    for (const voxel::DeltaChange& change : edit.changes) {
        CHECK(same(client.delta(change.coord), change.after));
    }

    // Befehl 11 gräbt beim Client woanders, der Server nicht (Strahl traf eine Kiste): Die
    // Bestätigung nimmt die Grube zurück.
    receiver.predict(client.terrain, 11, dig_brush(-20.0f, -20.0f));
    const usize with_prediction = world::terrain_deltas(client.terrain)->size();
    CHECK(with_prediction > edit.changes.size());
    REQUIRE(receiver.receive(replication::encode_terrain(
        {.terrain = 77, .acknowledged = 11, .complete = true, .chunks = {}})));
    CHECK(client.apply(receiver) == 1);
    CHECK(receiver.stats().mismatched == 1);
    CHECK(world::terrain_deltas(client.terrain)->size() == edit.changes.size());

    // Offene Pinsel nach der Bestätigung bleiben über Deltas des Servers hinweg stehen.
    receiver.predict(client.terrain, 13, dig_brush(-20.0f, -20.0f));
    const voxel::TerrainEdit other = world::edit_terrain(server.terrain, dig_brush(48.0f, 8.0f));
    replication::TerrainMessage foreign{.terrain = 77, .acknowledged = 12, .complete = true,
                                        .chunks = {}};
    for (const voxel::DeltaChange& change : other.changes) {
        foreign.chunks.push_back({change.coord, change.after});
    }
    REQUIRE(receiver.receive(replication::encode_terrain(foreign)));
    CHECK(client.apply(receiver) == 1);
    CHECK(receiver.stats().predicted == 1);
    CHECK(world::terrain_deltas(client.terrain)->size() >
          world::terrain_deltas(server.terrain)->size());

    // Bestätigt der Server lange nicht, verfällt der Pinsel.
    receiver.expire(client.world, 13 + 59);
    CHECK(receiver.stats().predicted == 1);
    receiver.expire(client.world, 13 + 60);
    CHECK(receiver.stats().predicted == 0);
    CHECK(receiver.stats().expired == 1);
    CHECK(world::terrain_deltas(client.terrain)->size() ==
          world::terrain_deltas(server.terrain)->size());
}
