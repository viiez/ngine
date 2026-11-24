module;


#include <ngine/profile.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


// Schnappschüsse des Servers (SnapshotEncoder): Stand der Welt erfassen, pro Client das Delta
// gegen den bestätigten Stand mit Relevanz und Budget (R5.2, ADR 0052). Bis R5.3 in snapshot.cpp.
module ngine.replication;


import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.reflect;
import ngine.serialize;
import ngine.world;
import :wire;

namespace ngine::replication {

using namespace wire;
using serialize::BinaryReader;
using serialize::BinaryWriter;

// --- Server ----------------------------------------------------------------------------------

namespace {

struct Captured {
    RecordPtr record;
    math::DVec3 position{0.0, 0.0, 0.0};
    // Für jeden Client relevant (ohne Lage oder mit einer globalen Komponente).
    bool global = false;
};

struct EncoderClient {
    std::deque<SentView> history;
    std::optional<u32> acked;
    std::optional<math::DVec3> focus;
    // Geänderte Entities, die warten müssen: seit welchem Tick.
    std::unordered_map<u64, u32> waiting;
    std::deque<usize> sizes;
    EncoderClientStats stats;
};

} // namespace

struct SnapshotEncoder::Impl {
    world::World* world = nullptr;
    const Schema* schema = nullptr;
    EncoderSettings settings;
    std::vector<world::ComponentId> ids;
    std::vector<world::ComponentId> global;
    world::ComponentId world_transform{};
    std::vector<Captured> state;
    u32 tick = 0;
    std::unordered_map<u32, EncoderClient> clients;
};

SnapshotEncoder::SnapshotEncoder(world::World& world, const Schema& schema,
                                 EncoderSettings settings)
    : impl_{std::make_unique<Impl>()} {
    impl_->world = &world;
    impl_->schema = &schema;
    impl_->settings = settings;
    impl_->ids = component_ids(world, schema);
    impl_->world_transform = world.component<world::WorldTransform>();
}

SnapshotEncoder::~SnapshotEncoder() = default;

void SnapshotEncoder::set_global(std::span<const reflect::TypeInfo* const> types) {
    impl_->global.clear();
    for (const reflect::TypeInfo* type : types) {
        impl_->global.push_back(impl_->world->component(*type));
    }
}

void SnapshotEncoder::add_client(u32 client) {
    impl_->clients[client] = EncoderClient{};
}

void SnapshotEncoder::remove_client(u32 client) {
    impl_->clients.erase(client);
}

void SnapshotEncoder::set_focus(u32 client, std::optional<math::DVec3> focus) {
    if (const auto it = impl_->clients.find(client); it != impl_->clients.end()) {
        it->second.focus = focus;
    }
}

void SnapshotEncoder::acknowledge(u32 client, u32 tick) {
    const auto it = impl_->clients.find(client);
    if (it == impl_->clients.end()) {
        return;
    }
    EncoderClient& state = it->second;
    if (!state.acked || newer(tick, *state.acked)) {
        state.acked = tick;
        state.stats.acked_tick = tick;
    }
}

void SnapshotEncoder::capture(u32 tick) {
    NGINE_PROFILE_ZONE("replication::SnapshotEncoder::capture");
    Impl& d = *impl_;
    world::World& world = *d.world;
    d.tick = tick;
    std::vector<Captured> next;
    next.reserve(d.state.size());
    usize old = 0;
    for (const u64 entity : world.entity_ids()) {
        std::shared_ptr<EntityRecord> record = capture_record(world, *d.schema, d.ids, entity);
        if (!record) {
            continue;
        }
        Captured captured;
        for (const world::ComponentId id : d.global) {
            captured.global = captured.global || world.has(entity, id);
        }
        if (const auto* transform =
                static_cast<const world::WorldTransform*>(world.get(entity, d.world_transform))) {
            captured.position = transform->position();
        } else {
            captured.global = true;
        }
        // Unveränderte Stände teilen sich den Speicher; Deltas erkennen sie am Zeiger.
        while (old < d.state.size() && d.state[old].record->id < entity) {
            ++old;
        }
        if (old < d.state.size() && d.state[old].record->id == entity &&
            same_record(*d.state[old].record, *record)) {
            captured.record = d.state[old].record;
        } else {
            captured.record = std::move(record);
        }
        next.push_back(std::move(captured));
    }
    d.state = std::move(next);
}

std::vector<std::byte> SnapshotEncoder::encode(u32 client) {
    NGINE_PROFILE_ZONE("replication::SnapshotEncoder::encode");
    Impl& d = *impl_;
    const auto found = d.clients.find(client);
    verify(found != d.clients.end(), "Schnappschuss für einen unbekannten Client");
    EncoderClient& state = found->second;
    const EncoderSettings& settings = d.settings;

    std::shared_ptr<const View> base_view;
    u32 base_tick = 0;
    if (state.acked) {
        for (const SentView& sent : state.history) {
            if (sent.tick == *state.acked) {
                base_view = sent.view;
                base_tick = sent.tick;
            }
        }
    }
    const bool has_base = base_view != nullptr;
    if (!has_base) {
        ++state.stats.full;
        // Ohne Basis zählt kein Warten: Bis zur Bestätigung geht immer dieselbe nächste Auswahl.
        state.waiting.clear();
    }
    const View empty;
    const View& base = has_base ? *base_view : empty;

    struct Candidate {
        RecordPtr record;
        RecordPtr old;
        f64 distance = 0.0;
        u32 waited = 0;
    };
    std::vector<Candidate> candidates;
    std::vector<u64> removed;
    View next;
    next.reserve(base.size() + 16);
    usize b = 0;
    for (const Captured& captured : d.state) {
        const u64 id = captured.record->id;
        while (b < base.size() && base[b]->id < id) {
            removed.push_back(base[b]->id);
            state.waiting.erase(base[b]->id);
            ++b;
        }
        RecordPtr old;
        if (b < base.size() && base[b]->id == id) {
            old = base[b];
            ++b;
        }
        f64 distance = 0.0;
        if (!captured.global && state.focus) {
            distance = math::length(captured.position - *state.focus);
            if (distance > settings.relevance) {
                if (old) {
                    removed.push_back(id);
                }
                state.waiting.erase(id);
                continue;
            }
        }
        if (old && (old == captured.record || same_record(*old, *captured.record))) {
            next.push_back(old);
            state.waiting.erase(id);
            continue;
        }
        u32 waited = 0;
        if (has_base) {
            const auto [entry, inserted] = state.waiting.try_emplace(id, d.tick);
            waited = d.tick - entry->second;
        }
        // Ferne Entities nur jeden far_interval-ten Schnappschuss, versetzt nach Kennung.
        const bool far = distance > settings.near && settings.far_interval > 1 &&
                         (d.tick + static_cast<u32>(id)) % settings.far_interval != 0;
        if (far && old) {
            next.push_back(old);
            continue;
        }
        candidates.push_back({captured.record, old, distance, waited});
    }
    while (b < base.size()) {
        removed.push_back(base[b]->id);
        state.waiting.erase(base[b]->id);
        ++b;
    }
    // Vorrang: wer am längsten wartet, dann wer am nächsten ist.
    std::ranges::sort(candidates, [](const Candidate& x, const Candidate& y) {
        if (x.waited != y.waited) {
            return x.waited > y.waited;
        }
        if (x.distance != y.distance) {
            return x.distance < y.distance;
        }
        return x.record->id < y.record->id;
    });

    BinaryWriter writer;
    writer.write(static_cast<u8>(MessageKind::Snapshot));
    writer.write(d.tick);
    writer.write(base_tick);
    write_varint(writer, removed.size());
    for (const u64 id : removed) {
        writer.write(id);
    }
    const usize fixed = writer.bytes().size() + 5;
    BinaryWriter entries;
    u64 written = 0;
    for (const Candidate& candidate : candidates) {
        if (written > 0 && fixed + entries.bytes().size() >= settings.budget) {
            if (candidate.old) {
                next.push_back(candidate.old);
            }
            continue;
        }
        write_entry(entries, *d.schema, candidate.old.get(), *candidate.record);
        next.push_back(candidate.record);
        state.waiting.erase(candidate.record->id);
        ++written;
    }
    write_varint(writer, written);
    writer.write_bytes(entries.bytes());

    std::ranges::sort(next, {}, [](const RecordPtr& record) { return record->id; });
    state.stats.entities = static_cast<u32>(next.size());
    state.history.push_back({d.tick, std::make_shared<const View>(std::move(next))});
    while (state.history.size() > settings.history) {
        state.history.pop_front();
    }
    std::vector<std::byte> message = writer.take();
    state.stats.last_bytes = message.size();
    state.sizes.push_back(message.size());
    while (state.sizes.size() > static_cast<usize>(snapshot_rate)) {
        state.sizes.pop_front();
    }
    state.stats.bytes_per_second = 0;
    for (const usize size : state.sizes) {
        state.stats.bytes_per_second += size;
    }
    state.stats.waiting = static_cast<u32>(state.waiting.size());
    return message;
}

EncoderClientStats SnapshotEncoder::stats(u32 client) const {
    const auto it = impl_->clients.find(client);
    return it != impl_->clients.end() ? it->second.stats : EncoderClientStats{};
}

usize SnapshotEncoder::entity_count() const noexcept {
    return impl_->state.size();
}

View SnapshotEncoder::captured() const {
    View view;
    view.reserve(impl_->state.size());
    for (const Captured& captured : impl_->state) {
        view.push_back(captured.record);
    }
    return view;
}

} // namespace ngine::replication
