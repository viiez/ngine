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


// Spiegel des Clients (Mirror): Schnappschüsse in die eigene Welt anwenden und fremde Entities
// interpoliert zeichnen (R5.2, ADR 0052). Bis R5.3 in snapshot.cpp.
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

// --- Client ----------------------------------------------------------------------------------

namespace {

struct Sample {
    u32 tick = 0;
    world::Transform transform;
};

struct Track {
    std::deque<Sample> samples;
    // Die gezeichnete Zeit ist am letzten Stand vorbei; bis zum nächsten bleibt er stehen.
    bool settled = false;
    // Abstand der Stände einer bewegten Entity in Ticks (Spitze, klingt langsam ab): Entfernte
    // kommen nur jeden dritten Tick, und Verlust oder Budget verzögern einzelne Stände.
    f64 gap = 1.0;
    // Zusätzliche Verzögerung dieser Entity in Ticks, folgt sanft aus gap.
    f64 extra = 0.0;
};

constexpr usize max_samples = 12;
// Länger ohne Stand heißt bei einer Entity, die zuletzt stand: Sie stand still (Deltas schicken
// Unverändertes nicht). Eine bewegte überbrückt Lücken bis bridge_gap (verlorene oder verzögerte
// Stände) und steht erst danach.
constexpr u32 hold_gap = 3;
constexpr u32 bridge_gap = 9;
// Höchstens so viele Ticks zusätzlicher Verzögerung pro Entity (300 ms).
constexpr f64 max_extra = 9.0;
constexpr u32 mirror_history = 32;

world::Transform blend(const world::Transform& a, const world::Transform& b, f64 t) {
    return world::Transform{
        .position = a.position + (b.position - a.position) * t,
        .rotation = math::slerp(a.rotation, b.rotation, static_cast<f32>(t)),
        .scale = a.scale + (b.scale - a.scale) * static_cast<f32>(t),
    };
}

} // namespace

struct Mirror::Impl {
    world::World* world = nullptr;
    const Schema* schema = nullptr;
    std::vector<world::ComponentId> ids;
    std::unordered_map<u64, u64> entities;
    std::shared_ptr<const View> current = std::make_shared<const View>();
    std::deque<SentView> history;
    u32 last_tick = 0;
    bool has_tick = false;
    std::unordered_map<u64, Track> tracks;
    f64 render_tick = 0.0;
    bool clock = false;
    // Entities mit Verweisen auf (noch) unbekannte Entities oder unbekanntem Elternteil.
    std::unordered_set<u64> unresolved;
    std::unordered_set<u64> orphans;
    MirrorStats stats;
    // Eigene Figur auf dem Server und die Komponenten, die der Client für sie selbst setzt.
    u64 predicted = 0;
    std::vector<u16> owned;

    u64 local(u64 server_id) const noexcept {
        const auto it = entities.find(server_id);
        return it == entities.end() ? 0 : it->second;
    }

    // Schreibt alle Teile einer Komponente in die Welt.
    Result<void> decode(u64 local_entity, const EntityRecord& record, usize slot, u32 tick,
                        bool sample) {
        const u16 index = record.components[slot];
        const SchemaComponent& component = schema->component(index);
        if (record.id == predicted && world->has(local_entity, ids[index]) &&
            std::ranges::find(owned, index) != owned.end()) {
            return {};
        }
        void* data = world->ensure(local_entity, ids[index]);
        Resolver resolve{.entities = &entities, .unresolved = false};
        for (u32 part = 0; part < component.parts; ++part) {
            if (auto result = decode_part(record.part(record.first_part[slot] + part), component,
                                          data, part, resolve);
                !result) {
                return std::unexpected(
                    std::format("Komponente {}: {}", component.type->name, result.error()));
            }
        }
        world->modified(local_entity, ids[index]);
        if (resolve.unresolved) {
            unresolved.insert(record.id);
        }
        if (sample && component.codec == Codec::Transform) {
            Track& track = tracks[record.id];
            const auto& transform = *static_cast<const world::Transform*>(data);
            if (!track.samples.empty()) {
                const u32 gap = tick - track.samples.back().tick;
                const usize n = track.samples.size();
                const bool moving =
                    n >= 2 && (track.samples[n - 1].transform.position !=
                                   track.samples[n - 2].transform.position ||
                               track.samples[n - 1].transform.rotation !=
                                   track.samples[n - 2].transform.rotation);
                if (gap > hold_gap && (!moving || gap > bridge_gap)) {
                    track.samples.push_back({tick - 1, track.samples.back().transform});
                } else {
                    track.gap = std::max(static_cast<f64>(gap), track.gap - 0.1);
                }
            }
            track.samples.push_back({tick, transform});
            while (track.samples.size() > max_samples) {
                track.samples.pop_front();
            }
            track.settled = false;
        }
        return {};
    }

    // Gleicht die Welt vom Stand current auf next ab.
    Result<void> sync(const View& next, u32 tick) {
        world::World& w = *world;
        const View& previous = *current;
        // Entities, die fehlen, enden.
        usize n = 0;
        for (const RecordPtr& old : previous) {
            while (n < next.size() && next[n]->id < old->id) {
                ++n;
            }
            if (n < next.size() && next[n]->id == old->id) {
                continue;
            }
            if (const u64 id = local(old->id); id != 0 && w.alive(id)) {
                w.destroy(id);
            }
            entities.erase(old->id);
            tracks.erase(old->id);
            unresolved.erase(old->id);
            orphans.erase(old->id);
        }
        for (const RecordPtr& record : next) {
            const RecordPtr* before = find_record(previous, record->id);
            if (before != nullptr && *before == record) {
                // Unverändert, außer die Entity endete hier (etwa mit ihrem Elternteil).
                if (const u64 id = local(record->id); id != 0 && w.alive(id)) {
                    continue;
                }
            }
            u64& entity = entities[record->id];
            bool fresh = false;
            if (entity == 0 || !w.alive(entity)) {
                entity = w.create().set(Replicated{record->id}).id();
                fresh = true;
            }
            const u64 local_entity = entity;
            const EntityRecord* old = before != nullptr && !fresh ? before->get() : nullptr;
            if (old != nullptr) {
                for (const u16 component : old->components) {
                    if (record->slot(component) < 0 && w.has(local_entity, ids[component])) {
                        w.remove(local_entity, ids[component]);
                    }
                }
            }
            for (usize slot = 0; slot < record->components.size(); ++slot) {
                const u16 component = record->components[slot];
                const i32 old_slot = old != nullptr ? old->slot(component) : -1;
                bool same = old_slot >= 0;
                for (u32 part = 0; same && part < schema->component(component).parts; ++part) {
                    same = same_part(*old, old->first_part[static_cast<usize>(old_slot)] + part,
                                     *record, record->first_part[slot] + part);
                }
                if (same) {
                    continue;
                }
                if (auto result = decode(local_entity, *record, slot, tick, true); !result) {
                    return std::unexpected(
                        std::format("Entity {}: {}", record->id, result.error()));
                }
            }
            if (old == nullptr || old->parent != record->parent) {
                orphans.insert(record->id);
            }
        }
        current = std::make_shared<const View>(next);
        // Elternteile, sobald beide da sind.
        for (auto it = orphans.begin(); it != orphans.end();) {
            const RecordPtr* record = find_record(*current, *it);
            const u64 child = local(*it);
            if (record == nullptr || child == 0 || !w.alive(child)) {
                it = orphans.erase(it);
                continue;
            }
            const u64 parent = (*record)->parent != 0 ? local((*record)->parent) : 0;
            if (w.parent(child) != parent) {
                w.set_parent(child, parent);
            }
            it = (*record)->parent == 0 || parent != 0 ? orphans.erase(it) : std::next(it);
        }
        // Verweise auf Entities, die jetzt da sein können.
        std::unordered_set<u64> retry;
        retry.swap(unresolved);
        for (const u64 server_id : retry) {
            const RecordPtr* record = find_record(*current, server_id);
            const u64 entity = local(server_id);
            if (record == nullptr || entity == 0 || !w.alive(entity)) {
                continue;
            }
            for (usize slot = 0; slot < (*record)->components.size(); ++slot) {
                if (auto result = decode(entity, **record, slot, tick, false); !result) {
                    return std::unexpected(result.error());
                }
            }
        }
        return {};
    }
};

Mirror::Mirror(world::World& world, const Schema& schema) : impl_{std::make_unique<Impl>()} {
    impl_->world = &world;
    impl_->schema = &schema;
    impl_->ids = component_ids(world, schema);
}

Mirror::~Mirror() = default;

Result<bool> Mirror::apply(std::span<const std::byte> payload) {
    NGINE_PROFILE_ZONE("replication::Mirror::apply");
    Impl& d = *impl_;
    BinaryReader reader{payload};
    u32 tick = 0;
    u32 base_tick = 0;
    if (!reader.read(tick) || !reader.read(base_tick)) {
        return std::unexpected("Schnappschuss ist unvollständig");
    }
    if (d.has_tick && !newer(tick, d.last_tick)) {
        ++d.stats.stale;
        return false;
    }
    std::shared_ptr<const View> base;
    if (base_tick != 0) {
        for (const SentView& sent : d.history) {
            if (sent.tick == base_tick) {
                base = sent.view;
            }
        }
        if (!base) {
            ++d.stats.missing_base;
            return false;
        }
    }
    const View empty;
    const View& base_view = base ? *base : empty;

    u64 removed_count = 0;
    if (!read_varint(reader, removed_count) || removed_count > reader.remaining() / 8) {
        return std::unexpected("Schnappschuss ist unvollständig");
    }
    std::unordered_set<u64> removed;
    for (u64 i = 0; i < removed_count; ++i) {
        u64 id = 0;
        if (!reader.read(id)) {
            return std::unexpected("Schnappschuss ist unvollständig");
        }
        removed.insert(id);
    }
    u64 entry_count = 0;
    if (!read_varint(reader, entry_count) || entry_count > reader.remaining()) {
        return std::unexpected("Schnappschuss ist unvollständig");
    }
    View changed;
    changed.reserve(entry_count);
    for (u64 i = 0; i < entry_count; ++i) {
        auto record = read_entry(reader, payload, *d.schema, base_view);
        if (!record) {
            return std::unexpected(std::format("Schnappschuss {}: {}", tick, record.error()));
        }
        changed.push_back(std::move(*record));
    }
    if (reader.remaining() != 0) {
        return std::unexpected("Schnappschuss hat überzählige Bytes");
    }
    std::ranges::sort(changed, {}, [](const RecordPtr& record) { return record->id; });

    // Neuer Stand: Basis ohne Entfernte, Geänderte ersetzen oder kommen hinzu.
    View next;
    next.reserve(base_view.size() + changed.size());
    usize c = 0;
    for (const RecordPtr& record : base_view) {
        while (c < changed.size() && changed[c]->id < record->id) {
            next.push_back(changed[c++]);
        }
        if (c < changed.size() && changed[c]->id == record->id) {
            next.push_back(changed[c++]);
        } else if (!removed.contains(record->id)) {
            next.push_back(record);
        }
    }
    while (c < changed.size()) {
        next.push_back(changed[c++]);
    }

    if (auto result = d.sync(next, tick); !result) {
        return std::unexpected(result.error());
    }
    d.history.push_back({tick, d.current});
    while (d.history.size() > mirror_history) {
        d.history.pop_front();
    }
    d.last_tick = tick;
    d.has_tick = true;
    return true;
}

void Mirror::interpolate(f64 dt, f64 delay) {
    NGINE_PROFILE_ZONE("replication::Mirror::interpolate");
    Impl& d = *impl_;
    const auto transform_index = d.schema->transform_index();
    if (!d.has_tick || !transform_index) {
        return;
    }
    // Die Uhr folgt dem neuesten Schnappschuss mit delay Abstand und stellt sich sanft nach.
    const f64 newest = static_cast<f64>(d.last_tick);
    const f64 target = newest - delay;
    if (!d.clock || std::abs(target - d.render_tick) > 10.0) {
        d.render_tick = target;
        d.clock = true;
    } else {
        const f64 error = target - d.render_tick;
        d.render_tick += dt * snapshot_rate * (1.0 + std::clamp(error * 0.1, -0.1, 0.1));
    }
    // Höchstens 100 ms über den neuesten Stand hinaus.
    d.render_tick = std::min(d.render_tick, newest + 3.0);
    const bool beyond = d.render_tick > newest;
    if (beyond) {
        ++d.stats.extrapolated;
    }
    d.stats.buffer_ticks = newest - d.render_tick;

    world::World& world = *d.world;
    const world::ComponentId transform_id = d.ids[*transform_index];
    const f64 render = d.render_tick;
    u32 moving = 0;
    for (auto& [server_id, track] : d.tracks) {
        if (track.samples.empty() || (track.settled && !beyond) || server_id == d.predicted) {
            continue;
        }
        const u64 entity = d.local(server_id);
        auto* transform = entity != 0 && world.alive(entity)
                              ? static_cast<world::Transform*>(world.get_mut(entity, transform_id))
                              : nullptr;
        if (transform == nullptr) {
            continue;
        }
        // Seltene oder unregelmäßige Stände: diese Entity etwas später zeichnen, damit sie zwischen
        // zwei Ständen liegt, statt vorauszurechnen. Die Verzögerung ändert sich um höchstens 10 %
        // der Zeit, damit die Entity dabei nicht springt.
        const f64 target_extra = std::clamp(2.0 * track.gap - 4.0, 0.0, max_extra);
        const f64 step = dt * snapshot_rate * 0.1;
        track.extra += std::clamp(target_extra - track.extra, -step, step);
        const f64 at = render - track.extra;
        const std::deque<Sample>& samples = track.samples;
        world::Transform value = samples.back().transform;
        if (at <= static_cast<f64>(samples.front().tick)) {
            value = samples.front().transform;
        } else if (at >= static_cast<f64>(samples.back().tick)) {
            // Fehlen Schnappschüsse, läuft eine bewegte Entity kurz weiter; sonst steht sie.
            const Sample& last = samples.back();
            if (beyond && last.tick == d.last_tick && samples.size() >= 2) {
                const Sample& before = samples[samples.size() - 2];
                const f64 span = static_cast<f64>(last.tick - before.tick);
                if (span > 0.0 && span <= hold_gap) {
                    const f64 t = (at - static_cast<f64>(last.tick)) / span;
                    value = last.transform;
                    value.position = last.transform.position +
                                     (last.transform.position - before.transform.position) * t;
                }
            } else {
                track.settled = true;
            }
        } else {
            for (usize i = 0; i + 1 < samples.size(); ++i) {
                const f64 a = static_cast<f64>(samples[i].tick);
                const f64 b = static_cast<f64>(samples[i + 1].tick);
                if (at >= a && at < b) {
                    value = blend(samples[i].transform, samples[i + 1].transform,
                                  (at - a) / (b - a));
                    break;
                }
            }
        }
        *transform = value;
        world.modified(entity, transform_id);
        ++moving;
    }
    d.stats.interpolated = moving;
}

void Mirror::set_predicted(u64 server_id, std::span<const reflect::TypeInfo* const> owned) {
    Impl& d = *impl_;
    d.predicted = server_id;
    d.owned.clear();
    for (const reflect::TypeInfo* type : owned) {
        if (type == nullptr) {
            continue;
        }
        if (const auto index = d.schema->index_of(*type)) {
            d.owned.push_back(*index);
        }
    }
    d.tracks.erase(server_id);
}

u64 Mirror::local_id(u64 server_id) const noexcept {
    return impl_->local(server_id);
}

usize Mirror::entity_count() const noexcept {
    return impl_->entities.size();
}

u32 Mirror::last_tick() const noexcept {
    return impl_->last_tick;
}

f64 Mirror::render_tick() const noexcept {
    return impl_->render_tick;
}

MirrorStats Mirror::stats() const noexcept {
    return impl_->stats;
}

const View& Mirror::view() const noexcept {
    return *impl_->current;
}

void Mirror::clear() {
    Impl& d = *impl_;
    for (const auto& [server_id, local] : d.entities) {
        if (d.world->alive(local)) {
            d.world->destroy(local);
        }
    }
    d.entities.clear();
    d.current = std::make_shared<const View>();
    d.history.clear();
    d.tracks.clear();
    d.unresolved.clear();
    d.orphans.clear();
    d.has_tick = false;
    d.last_tick = 0;
    d.clock = false;
}

} // namespace ngine::replication
