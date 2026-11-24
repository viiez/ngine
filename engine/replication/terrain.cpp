module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module ngine.replication;

import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.serialize;
import ngine.world;
import :wire;

namespace ngine::replication {

using serialize::BinaryReader;
using serialize::BinaryWriter;
using wire::read_varint;
using wire::write_varint;

namespace {

constexpr u8 flag_complete = 1;

u64 zigzag(i32 value) noexcept {
    const u32 bits = static_cast<u32>(value);
    return static_cast<u64>((bits << 1) ^ (value < 0 ? 0xFFFF'FFFFu : 0u));
}

i32 unzigzag(u64 value) noexcept {
    return static_cast<i32>(static_cast<u32>(value >> 1) ^ (0u - static_cast<u32>(value & 1)));
}

bool same_delta(const std::shared_ptr<const voxel::ChunkDelta>& a,
                const std::shared_ptr<const voxel::ChunkDelta>& b) noexcept {
    if (a == b) {
        return true;
    }
    const bool a_empty = !a || a->empty();
    const bool b_empty = !b || b->empty();
    if (a_empty || b_empty) {
        return a_empty == b_empty;
    }
    return *a == *b;
}

// Befehl a liegt nicht nach b (Nummern zählen endlos weiter).
bool not_after(u32 a, u32 b) noexcept {
    return static_cast<i32>(a - b) <= 0;
}

// Mittelpunkt eines Chunks der Stufe 0 in der Welt.
math::DVec3 chunk_center(math::DVec3 origin, voxel::ChunkCoord c) noexcept {
    constexpr f64 half = voxel::chunk_voxels / 2.0;
    return origin + math::DVec3{static_cast<f64>(voxel::chunk_origin(c.x)) + half,
                                static_cast<f64>(voxel::chunk_origin(c.y)) + half,
                                static_cast<f64>(voxel::chunk_origin(c.z)) + half} *
                        voxel::voxel_size;
}

// Bytes eines Chunks in der Nachricht (Obergrenze der Varints).
usize chunk_bytes(const std::shared_ptr<const voxel::ChunkDelta>& delta) noexcept {
    const usize size = delta ? delta->bytes().size() : 0;
    return 3 * 5 + 5 + size;
}

} // namespace

// --- Nachricht ------------------------------------------------------------------------------

std::vector<std::byte> encode_terrain(const TerrainMessage& message) {
    BinaryWriter writer;
    writer.write(static_cast<u8>(GameMessage::Terrain));
    writer.write(message.terrain);
    writer.write(message.acknowledged);
    writer.write(static_cast<u8>(message.complete ? flag_complete : 0));
    write_varint(writer, message.chunks.size());
    for (const TerrainChunk& chunk : message.chunks) {
        write_varint(writer, zigzag(chunk.coord.x));
        write_varint(writer, zigzag(chunk.coord.y));
        write_varint(writer, zigzag(chunk.coord.z));
        const auto bytes = chunk.delta ? chunk.delta->bytes() : std::span<const std::byte>{};
        write_varint(writer, bytes.size());
        writer.write_bytes(bytes);
    }
    return writer.take();
}

Result<TerrainMessage> decode_terrain(std::span<const std::byte> data) {
    if (data.empty() || static_cast<GameMessage>(data[0]) != GameMessage::Terrain) {
        return std::unexpected("Keine Nachricht des Terrains");
    }
    BinaryReader reader{data.subspan(1)};
    TerrainMessage message;
    u8 flags = 0;
    u64 count = 0;
    if (!reader.read(message.terrain) || !reader.read(message.acknowledged) ||
        !reader.read(flags) || !read_varint(reader, count)) {
        return std::unexpected("Nachricht des Terrains ist unvollständig");
    }
    if ((flags & ~flag_complete) != 0) {
        return std::unexpected("Nachricht des Terrains hat unbekannte Flags");
    }
    message.complete = (flags & flag_complete) != 0;
    // Jeder Chunk braucht mindestens vier Bytes; mehr kann nicht folgen.
    if (count > reader.remaining() / 4) {
        return std::unexpected("Nachricht des Terrains nennt zu viele Chunks");
    }
    message.chunks.reserve(count);
    std::vector<std::byte> bytes;
    for (u64 i = 0; i < count; ++i) {
        u64 x = 0;
        u64 y = 0;
        u64 z = 0;
        u64 size = 0;
        if (!read_varint(reader, x) || !read_varint(reader, y) || !read_varint(reader, z) ||
            !read_varint(reader, size)) {
            return std::unexpected("Chunk in der Nachricht des Terrains ist unvollständig");
        }
        if (x > 0xFFFF'FFFFu || y > 0xFFFF'FFFFu || z > 0xFFFF'FFFFu) {
            return std::unexpected("Koordinate eines Chunks liegt außerhalb des Bereichs");
        }
        if (size > reader.remaining()) {
            return std::unexpected("Delta in der Nachricht des Terrains ist abgeschnitten");
        }
        TerrainChunk chunk{.coord = {unzigzag(x), unzigzag(y), unzigzag(z), 0}, .delta = nullptr};
        if (size > 0) {
            bytes.resize(size);
            if (!reader.read_bytes(bytes)) {
                return std::unexpected("Delta in der Nachricht des Terrains ist abgeschnitten");
            }
            auto delta = voxel::ChunkDelta::decode(bytes);
            if (!delta) {
                return std::unexpected(delta.error());
            }
            if (!delta->empty()) {
                chunk.delta = std::make_shared<const voxel::ChunkDelta>(std::move(*delta));
            }
        }
        message.chunks.push_back(std::move(chunk));
    }
    if (reader.remaining() != 0) {
        return std::unexpected("Nachricht des Terrains hat überzählige Bytes");
    }
    return message;
}

// --- Server ---------------------------------------------------------------------------------

TerrainSender::TerrainSender(TerrainSenderSettings settings) : settings_{settings} {}

void TerrainSender::mark_all(ClientTerrain& client, const voxel::DeltaMap& deltas) const {
    for (const auto& [coord, delta] : deltas) {
        client.pending.insert(coord);
    }
    // Chunks, die der Client noch mit Delta kennt, die es nicht mehr gibt.
    for (const auto& [coord, delta] : client.sent) {
        if (!deltas.contains(coord)) {
            client.pending.insert(coord);
        }
    }
}

void TerrainSender::add_client(u32 client) {
    ClientEntry& entry = clients_[client];
    entry = {};
    for (const auto& [id, terrain] : terrains_) {
        mark_all(entry.terrains[id], *terrain.deltas);
    }
}

void TerrainSender::remove_client(u32 client) {
    clients_.erase(client);
}

void TerrainSender::set_focus(u32 client, std::optional<math::DVec3> focus) {
    if (const auto it = clients_.find(client); it != clients_.end()) {
        it->second.focus = focus;
    }
}

void TerrainSender::update_terrain(u64 terrain, std::shared_ptr<const voxel::DeltaMap> deltas,
                                   math::DVec3 origin) {
    if (!deltas) {
        deltas = std::make_shared<const voxel::DeltaMap>();
    }
    auto [it, inserted] = terrains_.try_emplace(terrain);
    TerrainEntry& entry = it->second;
    entry.origin = origin;
    if (!inserted && entry.deltas == deltas) {
        return;
    }
    NGINE_PROFILE_ZONE("replication::TerrainSender::update_terrain");
    // Geänderte Chunks: unveränderte Einträge teilen den Zeiger mit der vorigen Momentaufnahme.
    std::vector<voxel::ChunkCoord> changed;
    if (inserted || !entry.deltas) {
        for (const auto& [coord, delta] : *deltas) {
            changed.push_back(coord);
        }
    } else {
        for (const auto& [coord, delta] : *deltas) {
            const auto old = entry.deltas->find(coord);
            if (old == entry.deltas->end() || old->second != delta) {
                changed.push_back(coord);
            }
        }
        for (const auto& [coord, delta] : *entry.deltas) {
            if (!deltas->contains(coord)) {
                changed.push_back(coord);
            }
        }
    }
    entry.deltas = std::move(deltas);
    for (auto& [id, client] : clients_) {
        ClientTerrain& state = client.terrains[terrain];
        state.pending.insert(changed.begin(), changed.end());
    }
}

void TerrainSender::retain_terrains(std::span<const u64> keep) {
    std::erase_if(terrains_, [&](const auto& entry) {
        return std::ranges::find(keep, entry.first) == keep.end();
    });
    for (auto& [id, client] : clients_) {
        std::erase_if(client.terrains, [&](const auto& entry) {
            return std::ranges::find(keep, entry.first) == keep.end();
        });
    }
}

std::vector<std::vector<std::byte>> TerrainSender::encode(u32 client, u32 acknowledged,
                                                          bool force) {
    std::vector<std::vector<std::byte>> out;
    const auto found = clients_.find(client);
    if (found == clients_.end()) {
        return out;
    }
    NGINE_PROFILE_ZONE("replication::TerrainSender::encode");
    ClientEntry& entry = found->second;
    usize budget = settings_.budget;
    const f64 relevance_squared = settings_.relevance * settings_.relevance;
    for (auto& [id, terrain] : terrains_) {
        ClientTerrain& state = entry.terrains[id];
        // Vorgemerkte Chunks nach Abstand; was der Client schon so kennt, fällt weg.
        struct Candidate {
            voxel::ChunkCoord coord;
            std::shared_ptr<const voxel::ChunkDelta> delta;
            f64 distance = 0.0;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(state.pending.size());
        for (const voxel::ChunkCoord coord : state.pending) {
            const auto current = terrain.deltas->find(coord);
            std::shared_ptr<const voxel::ChunkDelta> delta =
                current != terrain.deltas->end() ? current->second : nullptr;
            const auto sent = state.sent.find(coord);
            const std::shared_ptr<const voxel::ChunkDelta> known =
                sent != state.sent.end() ? sent->second : nullptr;
            if (delta == known) {
                continue;
            }
            const f64 distance =
                entry.focus ? math::length_squared(chunk_center(terrain.origin, coord) - *entry.focus)
                            : relevance_squared * 4.0 + 1.0;
            candidates.push_back({coord, std::move(delta), distance});
        }
        std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
            if (a.distance != b.distance) {
                return a.distance < b.distance;
            }
            return std::tie(a.coord.x, a.coord.y, a.coord.z) < std::tie(b.coord.x, b.coord.y, b.coord.z);
        });
        TerrainMessage message{.terrain = id, .acknowledged = acknowledged, .complete = false,
                               .chunks = {}};
        bool near_left = false;
        bool budget_spent = false;
        for (Candidate& candidate : candidates) {
            const bool near = entry.focus && candidate.distance <= relevance_squared;
            const usize size = chunk_bytes(candidate.delta);
            if (!near) {
                // Mindestens ein Chunk pro Aufruf, damit große Deltas nicht ewig warten.
                if (budget_spent || (size > budget && budget < settings_.budget)) {
                    budget_spent = true;
                    continue;
                }
                budget = size >= budget ? 0 : budget - size;
            }
            state.pending.erase(candidate.coord);
            if (candidate.delta) {
                state.sent.insert_or_assign(candidate.coord, candidate.delta);
            } else {
                state.sent.erase(candidate.coord);
            }
            message.chunks.push_back({candidate.coord, std::move(candidate.delta)});
        }
        // Unveränderte Vormerkungen fallen weg, damit pending nur echte Arbeit zählt.
        std::erase_if(state.pending, [&](voxel::ChunkCoord coord) {
            const auto current = terrain.deltas->find(coord);
            const auto sent = state.sent.find(coord);
            const auto* delta = current != terrain.deltas->end() ? current->second.get() : nullptr;
            const auto* known = sent != state.sent.end() ? sent->second.get() : nullptr;
            return delta == known;
        });
        if (entry.focus) {
            for (const voxel::ChunkCoord coord : state.pending) {
                const f64 distance =
                    math::length_squared(chunk_center(terrain.origin, coord) - *entry.focus);
                if (distance <= relevance_squared) {
                    near_left = true;
                    break;
                }
            }
        }
        const bool announce = entry.focus && !near_left && !state.complete;
        if (announce) {
            state.complete = true;
            message.complete = true;
        } else {
            message.complete = state.complete && !near_left;
        }
        if (message.chunks.empty() && !announce && !force) {
            continue;
        }
        entry.stats.chunks += message.chunks.size();
        std::vector<std::byte> bytes = encode_terrain(message);
        entry.stats.bytes += bytes.size();
        if (!entry.stats.complete) {
            entry.stats.join_bytes += bytes.size();
        }
        if (announce) {
            entry.stats.complete = true;
        }
        out.push_back(std::move(bytes));
    }
    entry.stats.pending = 0;
    for (const auto& [id, state] : entry.terrains) {
        entry.stats.pending += state.pending.size();
    }
    return out;
}

TerrainClientStats TerrainSender::stats(u32 client) const {
    const auto found = clients_.find(client);
    return found != clients_.end() ? found->second.stats : TerrainClientStats{};
}

// --- Client ---------------------------------------------------------------------------------

Result<void> TerrainReceiver::receive(std::span<const std::byte> data) {
    auto message = decode_terrain(data);
    if (!message) {
        return std::unexpected(message.error());
    }
    stats_.bytes += data.size();
    receive(std::move(*message));
    return {};
}

void TerrainReceiver::receive(TerrainMessage message) {
    ++stats_.messages;
    stats_.chunks += message.chunks.size();
    queue_.push_back(std::move(message));
}

usize TerrainReceiver::apply(world::World& world, const Mirror& mirror) {
    return apply(world, [&mirror](u64 id) { return mirror.local_id(id); });
}

usize TerrainReceiver::apply(world::World& world, const std::function<u64(u64)>& local_id) {
    usize applied = 0;
    std::unordered_set<u64> blocked;
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (blocked.contains(it->terrain)) {
            ++it;
            continue;
        }
        const u64 local = local_id(it->terrain);
        world::Entity entity = local != 0 && world.alive(local) ? world.entity(local) : world::Entity{};
        if (!entity || world::terrain_deltas(entity) == nullptr) {
            blocked.insert(it->terrain);
            ++it;
            continue;
        }
        NGINE_PROFILE_ZONE("replication::TerrainReceiver::apply");
        // Eine neue Entity für dasselbe Terrain (der Spiegel legte sie neu an) bekommt alles
        // Bestätigte; vorhergesagte Pinsel für die alte verfallen.
        Known& terrain = terrains_[it->terrain];
        if (terrain.local != local) {
            if (terrain.local != 0) {
                std::erase_if(predictions_,
                              [&](const Prediction& p) { return p.terrain == it->terrain; });
                auto map = std::make_shared<voxel::DeltaMap>();
                for (const auto& [coord, delta] : terrain.confirmed) {
                    if (delta) {
                        map->emplace(coord, delta);
                    }
                }
                world::set_terrain_deltas(entity, std::move(map));
            }
            terrain.local = local;
        }
        reconcile(it->terrain, entity, it->chunks, it->acknowledged, false);
        if (it->complete) {
            complete_ = true;
        }
        it = queue_.erase(it);
        ++applied;
    }
    return applied;
}

void TerrainReceiver::predict(world::Entity terrain, u32 command, const voxel::Brush& brush) {
    const auto* replicated = terrain ? terrain.get<Replicated>() : nullptr;
    if (replicated == nullptr) {
        (void)world::edit_terrain(terrain, brush);
        return;
    }
    voxel::TerrainEdit edit = world::edit_terrain(terrain, brush);
    Known& state = terrains_[replicated->server_id];
    state.local = terrain.id();
    predictions_.push_back({.terrain = replicated->server_id,
                            .command = command,
                            .brush = brush,
                            .changes = std::move(edit.changes)});
}

void TerrainReceiver::expire(world::World& world, u32 acknowledged, u32 margin) {
    if (predictions_.empty()) {
        return;
    }
    const u32 until = acknowledged - margin;
    std::vector<u64> affected;
    for (const Prediction& p : predictions_) {
        if (not_after(p.command, until) && std::ranges::find(affected, p.terrain) == affected.end()) {
            affected.push_back(p.terrain);
        }
    }
    for (const u64 server : affected) {
        const Known& terrain = terrains_[server];
        if (terrain.local == 0 || !world.alive(terrain.local)) {
            std::erase_if(predictions_, [&](const Prediction& p) { return p.terrain == server; });
            continue;
        }
        reconcile(server, world.entity(terrain.local), {}, until, true);
    }
}

void TerrainReceiver::reconcile(u64 server, world::Entity entity,
                                std::span<const TerrainChunk> chunks,
                                std::optional<u32> acknowledged, bool expiring) {
    Known& terrain = terrains_[server];
    std::vector<voxel::ChunkCoord> touched;
    for (const TerrainChunk& chunk : chunks) {
        terrain.confirmed.insert_or_assign(chunk.coord, chunk.delta);
        touched.push_back(chunk.coord);
    }
    // Pinsel bis acknowledged hat der Server verarbeitet: Sein Ergebnis steht jetzt in confirmed.
    std::vector<Prediction> dropped;
    std::vector<Prediction> kept;
    for (Prediction& p : predictions_) {
        if (p.terrain == server && acknowledged && not_after(p.command, *acknowledged)) {
            dropped.push_back(std::move(p));
        } else {
            kept.push_back(std::move(p));
        }
    }
    predictions_ = std::move(kept);
    for (const Prediction& p : predictions_) {
        if (p.terrain != server) {
            continue;
        }
        for (const voxel::DeltaChange& change : p.changes) {
            touched.push_back(change.coord);
        }
    }
    for (const Prediction& p : dropped) {
        bool differs = false;
        for (const voxel::DeltaChange& change : p.changes) {
            touched.push_back(change.coord);
            // Vergleichen lässt sich nur, was kein späterer Pinsel noch einmal ändert.
            const bool later = std::ranges::any_of(predictions_, [&](const Prediction& q) {
                return q.terrain == server &&
                       std::ranges::any_of(q.changes, [&](const voxel::DeltaChange& c) {
                           return c.coord == change.coord;
                       });
            });
            if (!later && !same_delta(change.after, confirmed(server, change.coord))) {
                differs = true;
            }
        }
        if (expiring) {
            ++stats_.expired;
        } else {
            ++stats_.confirmed;
            if (differs) {
                ++stats_.mismatched;
            }
        }
    }
    std::ranges::sort(touched, [](voxel::ChunkCoord a, voxel::ChunkCoord b) {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    });
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    if (touched.empty()) {
        return;
    }
    // Alles Berührte auf den bestätigten Stand, dann die offenen Pinsel neu.
    const std::shared_ptr<const voxel::DeltaMap> current = world::terrain_deltas(entity);
    std::vector<voxel::DeltaChange> changes;
    changes.reserve(touched.size());
    for (const voxel::ChunkCoord coord : touched) {
        std::shared_ptr<const voxel::ChunkDelta> before;
        if (current) {
            if (const auto it = current->find(coord); it != current->end()) {
                before = it->second;
            }
        }
        changes.push_back({coord, std::move(before), confirmed(server, coord)});
    }
    world::restore_terrain(entity, changes, true);
    for (Prediction& p : predictions_) {
        if (p.terrain == server) {
            p.changes = world::edit_terrain(entity, p.brush).changes;
        }
    }
}

TerrainReceiverStats TerrainReceiver::stats() const noexcept {
    TerrainReceiverStats stats = stats_;
    stats.predicted = predictions_.size();
    stats.waiting = queue_.size();
    return stats;
}

std::shared_ptr<const voxel::ChunkDelta> TerrainReceiver::confirmed(u64 server_terrain,
                                                                   voxel::ChunkCoord coord) const {
    const auto terrain = terrains_.find(server_terrain);
    if (terrain == terrains_.end()) {
        return nullptr;
    }
    const auto it = terrain->second.confirmed.find(coord);
    return it != terrain->second.confirmed.end() ? it->second : nullptr;
}

void TerrainReceiver::clear() {
    queue_.clear();
    terrains_.clear();
    predictions_.clear();
    complete_ = false;
    stats_ = {};
}

} // namespace ngine::replication
