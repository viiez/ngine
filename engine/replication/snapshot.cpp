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

module ngine.replication;

import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.reflect;
import ngine.serialize;
import ngine.world;

namespace ngine::replication {

namespace {

using serialize::BinaryReader;
using serialize::BinaryWriter;

constexpr u8 flag_new = 1;
constexpr u8 flag_parent = 2;

void write_varint(BinaryWriter& writer, u64 value) {
    while (value >= 0x80) {
        writer.write(static_cast<u8>(value | 0x80));
        value >>= 7;
    }
    writer.write(static_cast<u8>(value));
}

[[nodiscard]] bool read_varint(BinaryReader& reader, u64& value) {
    value = 0;
    for (u32 shift = 0; shift < 64; shift += 7) {
        u8 byte = 0;
        if (!reader.read(byte)) {
            return false;
        }
        value |= static_cast<u64>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

// Vergleich mit Überlauf: Ticks zählen endlos weiter.
[[nodiscard]] bool newer(u32 a, u32 b) noexcept {
    return static_cast<i32>(a - b) > 0;
}

// --- Codec für world::Transform --------------------------------------------------------------

constexpr f64 cell_size = 64.0;
constexpr f64 inv_sqrt2 = 0.70710678118654752440;
constexpr u32 rotation_bits = 15;
constexpr f64 rotation_max = static_cast<f64>((1u << rotation_bits) - 1);

void encode_position(BinaryWriter& writer, math::DVec3 position) {
    const f64 values[3] = {position.x, position.y, position.z};
    for (const f64 value : values) {
        const f64 cell = std::floor(value / cell_size);
        const i32 index = static_cast<i32>(std::clamp(cell, -2147483648.0, 2147483647.0));
        const f64 offset = (value - static_cast<f64>(index) * cell_size) * 1000.0;
        writer.write(index);
        writer.write(static_cast<u16>(std::clamp(std::round(offset), 0.0, 65535.0)));
    }
}

[[nodiscard]] bool decode_position(BinaryReader& reader, math::DVec3& position) {
    f64 values[3] = {};
    for (f64& value : values) {
        i32 index = 0;
        u16 millimeters = 0;
        if (!reader.read(index) || !reader.read(millimeters)) {
            return false;
        }
        value = static_cast<f64>(index) * cell_size + static_cast<f64>(millimeters) / 1000.0;
    }
    position = {values[0], values[1], values[2]};
    return true;
}

// Drehung als die drei kleinsten Komponenten (je 15 Bit) und der Index der größten (2 Bit),
// zusammen 47 Bit in 6 Bytes.
void encode_rotation(BinaryWriter& writer, math::Quat rotation) {
    f64 q[4] = {rotation.x, rotation.y, rotation.z, rotation.w};
    const f64 length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length > 0.0) {
        for (f64& c : q) {
            c /= length;
        }
    } else {
        q[0] = q[1] = q[2] = 0.0;
        q[3] = 1.0;
    }
    u32 largest = 0;
    for (u32 i = 1; i < 4; ++i) {
        if (std::abs(q[i]) > std::abs(q[largest])) {
            largest = i;
        }
    }
    const f64 sign = q[largest] < 0.0 ? -1.0 : 1.0;
    u64 packed = largest;
    u32 shift = 2;
    for (u32 i = 0; i < 4; ++i) {
        if (i == largest) {
            continue;
        }
        const f64 normalized = std::clamp((q[i] * sign / inv_sqrt2) * 0.5 + 0.5, 0.0, 1.0);
        packed |= static_cast<u64>(std::round(normalized * rotation_max)) << shift;
        shift += rotation_bits;
    }
    for (u32 i = 0; i < 6; ++i) {
        writer.write(static_cast<u8>(packed >> (i * 8)));
    }
}

[[nodiscard]] bool decode_rotation(BinaryReader& reader, math::Quat& rotation) {
    u64 packed = 0;
    for (u32 i = 0; i < 6; ++i) {
        u8 byte = 0;
        if (!reader.read(byte)) {
            return false;
        }
        packed |= static_cast<u64>(byte) << (i * 8);
    }
    const u32 largest = static_cast<u32>(packed & 3);
    f64 q[4] = {};
    f64 sum = 0.0;
    u32 shift = 2;
    for (u32 i = 0; i < 4; ++i) {
        if (i == largest) {
            continue;
        }
        const u64 bits = (packed >> shift) & ((1u << rotation_bits) - 1);
        q[i] = (static_cast<f64>(bits) / rotation_max - 0.5) * 2.0 * inv_sqrt2;
        sum += q[i] * q[i];
        shift += rotation_bits;
    }
    q[largest] = std::sqrt(std::max(0.0, 1.0 - sum));
    rotation = {static_cast<f32>(q[0]), static_cast<f32>(q[1]), static_cast<f32>(q[2]),
                static_cast<f32>(q[3])};
    return true;
}

// --- Teile kodieren --------------------------------------------------------------------------

void encode_part(BinaryWriter& writer, const SchemaComponent& component, const void* data,
                 u32 part) {
    if (component.codec == Codec::Transform) {
        const auto& transform = *static_cast<const world::Transform*>(data);
        switch (part) {
        case 0: encode_position(writer, transform.position); return;
        case 1: encode_rotation(writer, transform.rotation); return;
        default:
            writer.write(transform.scale.x);
            writer.write(transform.scale.y);
            writer.write(transform.scale.z);
            return;
        }
    }
    const reflect::Field& field = *component.fields[part];
    serialize::write_binary(writer, static_cast<const std::byte*>(data) + field.offset,
                            *field.type);
}

// Übersetzt Kennungen des Servers in eigene; 0, wenn die Entity (noch) fehlt.
struct Resolver {
    const std::unordered_map<u64, u64>* entities = nullptr;
    bool unresolved = false;

    u64 operator()(u64 server_id) {
        if (server_id == 0) {
            return 0;
        }
        const auto it = entities->find(server_id);
        if (it == entities->end()) {
            unresolved = true;
            return 0;
        }
        return it->second;
    }
};

[[nodiscard]] Result<void> decode_part(std::span<const std::byte> bytes,
                                       const SchemaComponent& component, void* data, u32 part,
                                       Resolver& resolve) {
    BinaryReader reader{bytes};
    bool ok = true;
    if (component.codec == Codec::Transform) {
        auto& transform = *static_cast<world::Transform*>(data);
        switch (part) {
        case 0: ok = decode_position(reader, transform.position); break;
        case 1: ok = decode_rotation(reader, transform.rotation); break;
        default:
            ok = reader.read(transform.scale.x) && reader.read(transform.scale.y) &&
                 reader.read(transform.scale.z);
            break;
        }
    } else {
        const reflect::Field& field = *component.fields[part];
        void* target = static_cast<std::byte*>(data) + field.offset;
        if (auto result = serialize::read_binary(reader, target, *field.type); !result) {
            return std::unexpected(result.error());
        }
        if (field.hint == reflect::Hint::Entity && field.type->kind == reflect::Kind::UInt &&
            field.type->size == sizeof(u64)) {
            u64 value = 0;
            std::memcpy(&value, target, sizeof(value));
            value = resolve(value);
            std::memcpy(target, &value, sizeof(value));
        }
    }
    if (!ok) {
        return std::unexpected("Transform ist unvollständig");
    }
    if (reader.remaining() != 0) {
        return std::unexpected("Teil hat überzählige Bytes");
    }
    return {};
}

[[nodiscard]] bool same_record(const EntityRecord& a, const EntityRecord& b) noexcept {
    return a.parent == b.parent && a.components == b.components && a.part_ends == b.part_ends &&
           a.bytes == b.bytes;
}

// Gleiche Bytes in einem Teil zweier Stände.
[[nodiscard]] bool same_part(const EntityRecord& a, usize part_a, const EntityRecord& b,
                             usize part_b) noexcept {
    const auto x = a.part(part_a);
    const auto y = b.part(part_b);
    return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin());
}

[[nodiscard]] const RecordPtr* find_record(const View& view, u64 id) noexcept {
    const auto it =
        std::ranges::lower_bound(view, id, {}, [](const RecordPtr& r) { return r->id; });
    return it != view.end() && (*it)->id == id ? &*it : nullptr;
}

// Schreibt einen Eintrag: Unterschied von old (darf nullptr sein) zu record.
void write_entry(BinaryWriter& writer, const Schema& schema, const EntityRecord* old,
                 const EntityRecord& record) {
    writer.write(record.id);
    u8 flags = old == nullptr ? flag_new : 0;
    const bool parent = old == nullptr ? record.parent != 0 : old->parent != record.parent;
    if (parent) {
        flags |= flag_parent;
    }
    writer.write(flags);
    if (parent) {
        writer.write(record.parent);
    }
    // Entfernte Komponenten.
    std::vector<u16> removed;
    if (old != nullptr) {
        for (const u16 component : old->components) {
            if (record.slot(component) < 0) {
                removed.push_back(component);
            }
        }
    }
    write_varint(writer, removed.size());
    for (const u16 component : removed) {
        writer.write(component);
    }
    // Geänderte Komponenten mit Maske der Teile.
    struct Change {
        usize slot;
        u64 mask;
    };
    std::vector<Change> changes;
    for (usize slot = 0; slot < record.components.size(); ++slot) {
        const u16 component = record.components[slot];
        const u32 parts = schema.component(component).parts;
        const i32 old_slot = old != nullptr ? old->slot(component) : -1;
        u64 mask = 0;
        for (u32 part = 0; part < parts; ++part) {
            if (old_slot < 0 ||
                !same_part(*old, old->first_part[static_cast<usize>(old_slot)] + part, record,
                           record.first_part[slot] + part)) {
                mask |= u64{1} << part;
            }
        }
        if (mask != 0) {
            changes.push_back({slot, mask});
        }
    }
    write_varint(writer, changes.size());
    for (const Change& change : changes) {
        writer.write(record.components[change.slot]);
        write_varint(writer, change.mask);
        const u32 parts = schema.component(record.components[change.slot]).parts;
        for (u32 part = 0; part < parts; ++part) {
            if ((change.mask & (u64{1} << part)) == 0) {
                continue;
            }
            const auto bytes = record.part(record.first_part[change.slot] + part);
            write_varint(writer, bytes.size());
            writer.write_bytes(bytes);
        }
    }
}

// Liest einen Eintrag aus payload (über reader) und baut den neuen Stand aus der Basis.
[[nodiscard]] Result<RecordPtr> read_entry(BinaryReader& reader, std::span<const std::byte> payload,
                                           const Schema& schema, const View& base_view) {
    u64 id = 0;
    u8 flags = 0;
    if (!reader.read(id) || !reader.read(flags)) {
        return std::unexpected("Eintrag ist unvollständig");
    }
    const RecordPtr* found = (flags & flag_new) != 0 ? nullptr : find_record(base_view, id);
    if ((flags & flag_new) == 0 && found == nullptr) {
        return std::unexpected(std::format("Delta für Entity {}, die die Basis nicht kennt", id));
    }
    const EntityRecord* base = found != nullptr ? found->get() : nullptr;
    u64 parent = base != nullptr ? base->parent : 0;
    if ((flags & flag_parent) != 0 && !reader.read(parent)) {
        return std::unexpected("Eintrag ist unvollständig");
    }
    u64 removed_count = 0;
    if (!read_varint(reader, removed_count) || removed_count > schema.size()) {
        return std::unexpected("Eintrag ist unvollständig");
    }
    std::vector<u16> removed(removed_count);
    for (u16& component : removed) {
        if (!reader.read(component)) {
            return std::unexpected("Eintrag ist unvollständig");
        }
    }
    // Teile pro Komponente: Bytes aus der Basis oder aus payload, beide leben bis zum Ende.
    struct Parts {
        u16 component = 0;
        std::vector<std::span<const std::byte>> parts;
        std::vector<bool> present;
    };
    std::vector<Parts> result;
    if (base != nullptr) {
        for (usize slot = 0; slot < base->components.size(); ++slot) {
            const u16 component = base->components[slot];
            if (std::ranges::find(removed, component) != removed.end()) {
                continue;
            }
            const u32 count = schema.component(component).parts;
            Parts entry{
                .component = component, .parts = {}, .present = std::vector<bool>(count, true)};
            for (u32 part = 0; part < count; ++part) {
                entry.parts.push_back(base->part(base->first_part[slot] + part));
            }
            result.push_back(std::move(entry));
        }
    }
    u64 change_count = 0;
    if (!read_varint(reader, change_count) || change_count > schema.size()) {
        return std::unexpected("Eintrag ist unvollständig");
    }
    std::vector<std::byte> skipped;
    for (u64 c = 0; c < change_count; ++c) {
        u16 component = 0;
        u64 mask = 0;
        if (!reader.read(component) || component >= schema.size() || !read_varint(reader, mask)) {
            return std::unexpected("Schnappschuss nennt eine unbekannte Komponente");
        }
        const u32 count = schema.component(component).parts;
        auto it = std::ranges::find(result, component, &Parts::component);
        if (it == result.end()) {
            result.push_back({.component = component,
                              .parts = std::vector<std::span<const std::byte>>(count),
                              .present = std::vector<bool>(count, false)});
            it = result.end() - 1;
        }
        for (u32 part = 0; part < count; ++part) {
            if ((mask & (u64{1} << part)) == 0) {
                continue;
            }
            u64 size = 0;
            if (!read_varint(reader, size) || size > reader.remaining()) {
                return std::unexpected("Teil ist unvollständig");
            }
            const usize begin = reader.position();
            skipped.resize(size);
            if (!reader.read_bytes(skipped)) {
                return std::unexpected("Teil ist unvollständig");
            }
            it->parts[part] = payload.subspan(begin, size);
            it->present[part] = true;
        }
        if (count < 64 && (mask >> count) != 0) {
            return std::unexpected("Maske nennt Teile, die die Komponente nicht hat");
        }
    }
    std::ranges::sort(result, {}, &Parts::component);
    auto record = std::make_shared<EntityRecord>();
    record->id = id;
    record->parent = parent;
    for (const Parts& entry : result) {
        if (std::ranges::find(entry.present, false) != entry.present.end()) {
            return std::unexpected(std::format("Entity {}: Komponente {} ist unvollständig", id,
                                               schema.component(entry.component).type->name));
        }
        record->components.push_back(entry.component);
        record->first_part.push_back(static_cast<u32>(record->part_ends.size()));
        for (const auto bytes : entry.parts) {
            record->bytes.insert(record->bytes.end(), bytes.begin(), bytes.end());
            record->part_ends.push_back(static_cast<u32>(record->bytes.size()));
        }
    }
    return RecordPtr{std::move(record)};
}

// Kodiert alle replizierten Komponenten einer Entity, nullptr ohne solche.
[[nodiscard]] std::shared_ptr<EntityRecord> capture_record(world::World& world,
                                                           const Schema& schema,
                                                           std::span<const world::ComponentId> ids,
                                                           u64 entity) {
    std::shared_ptr<EntityRecord> record;
    BinaryWriter writer;
    for (usize i = 0; i < ids.size(); ++i) {
        const void* data = world.get(entity, ids[i]);
        if (data == nullptr) {
            continue;
        }
        if (!record) {
            record = std::make_shared<EntityRecord>();
            record->id = entity;
        }
        record->components.push_back(static_cast<u16>(i));
        record->first_part.push_back(static_cast<u32>(record->part_ends.size()));
        const SchemaComponent& component = schema.component(i);
        for (u32 part = 0; part < component.parts; ++part) {
            encode_part(writer, component, data, part);
            record->part_ends.push_back(static_cast<u32>(writer.bytes().size()));
        }
    }
    if (record) {
        record->bytes = writer.take();
        record->parent = world.parent(entity);
    }
    return record;
}

[[nodiscard]] std::vector<world::ComponentId> component_ids(world::World& world,
                                                            const Schema& schema) {
    std::vector<world::ComponentId> ids;
    ids.reserve(schema.size());
    for (const SchemaComponent& component : schema.components()) {
        ids.push_back(world.component(*component.type));
    }
    return ids;
}

} // namespace

i32 EntityRecord::slot(u16 component) const noexcept {
    const auto it = std::ranges::lower_bound(components, component);
    return it != components.end() && *it == component ? static_cast<i32>(it - components.begin())
                                                      : -1;
}

void register_types(reflect::TypeRegistry& types) {
    if (types.find<Replicated>() == nullptr) {
        types.add<Replicated>("Replicated")
            .transient()
            .field("server_id", &Replicated::server_id)
            .hint(reflect::Hint::Entity);
    }
}

std::vector<std::byte> encode_welcome(const Schema& schema) {
    BinaryWriter writer;
    writer.write(static_cast<u8>(MessageKind::Welcome));
    writer.write(protocol_version);
    writer.write(schema.hash());
    writer.write(static_cast<u16>(schema.size()));
    for (const SchemaComponent& component : schema.components()) {
        writer.write_string(component.type->name);
    }
    return writer.take();
}

Result<void> check_welcome(std::span<const std::byte> payload, const Schema& schema) {
    BinaryReader reader{payload};
    u32 version = 0;
    u64 hash = 0;
    u16 count = 0;
    if (!reader.read(version)) {
        return std::unexpected("Willkommensnachricht ist unvollständig");
    }
    if (version != protocol_version) {
        return std::unexpected(std::format("Der Server spricht Protokoll {}, der Client {}",
                                           version, protocol_version));
    }
    if (!reader.read(hash) || !reader.read(count)) {
        return std::unexpected("Willkommensnachricht ist unvollständig");
    }
    if (hash == schema.hash()) {
        return {};
    }
    // Erste abweichende Komponente für die Meldung.
    std::string name;
    for (u16 i = 0; i < count; ++i) {
        if (!reader.read_string(name)) {
            return std::unexpected("Willkommensnachricht ist unvollständig");
        }
        if (i >= schema.size()) {
            return std::unexpected(
                std::format("Der Server repliziert {} Komponenten, der Client {}; zuerst fehlt {}",
                            count, schema.size(), name));
        }
        if (name != schema.component(i).type->name) {
            return std::unexpected(std::format("Komponente {} auf dem Server, {} auf dem Client",
                                               name, schema.component(i).type->name));
        }
    }
    if (count != schema.size()) {
        return std::unexpected(std::format("Der Server repliziert {} Komponenten, der Client {}",
                                           count, schema.size()));
    }
    return std::unexpected("Gleiche Komponenten, aber andere Felder auf Server und Client");
}

std::vector<std::byte> encode_ack(u32 tick) {
    BinaryWriter writer;
    writer.write(static_cast<u8>(MessageKind::Ack));
    writer.write(tick);
    return writer.take();
}

std::optional<u32> decode_ack(std::span<const std::byte> payload) {
    BinaryReader reader{payload};
    u32 tick = 0;
    if (!reader.read(tick) || reader.remaining() != 0) {
        return std::nullopt;
    }
    return tick;
}

// --- Server ----------------------------------------------------------------------------------

namespace {

struct Captured {
    RecordPtr record;
    math::DVec3 position{0.0, 0.0, 0.0};
    // Für jeden Client relevant (ohne Lage oder mit einer globalen Komponente).
    bool global = false;
};

struct SentView {
    u32 tick = 0;
    std::shared_ptr<const View> view;
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
};

constexpr usize max_samples = 8;
// Länger ohne Stand heißt: Die Entity stand still (Deltas schicken Unverändertes nicht).
constexpr u32 hold_gap = 3;
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

    u64 local(u64 server_id) const noexcept {
        const auto it = entities.find(server_id);
        return it == entities.end() ? 0 : it->second;
    }

    // Schreibt alle Teile einer Komponente in die Welt.
    Result<void> decode(u64 local_entity, const EntityRecord& record, usize slot, u32 tick,
                        bool sample) {
        const u16 index = record.components[slot];
        const SchemaComponent& component = schema->component(index);
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
            if (!track.samples.empty() && tick - track.samples.back().tick > hold_gap) {
                track.samples.push_back({tick - 1, track.samples.back().transform});
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
                continue;
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
            if (record == nullptr || child == 0) {
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
            if (record == nullptr || entity == 0) {
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
        if (track.samples.empty() || (track.settled && !beyond)) {
            continue;
        }
        const u64 entity = d.local(server_id);
        auto* transform = entity != 0
                              ? static_cast<world::Transform*>(world.get_mut(entity, transform_id))
                              : nullptr;
        if (transform == nullptr) {
            continue;
        }
        const std::deque<Sample>& samples = track.samples;
        world::Transform value = samples.back().transform;
        if (render <= static_cast<f64>(samples.front().tick)) {
            value = samples.front().transform;
        } else if (render >= static_cast<f64>(samples.back().tick)) {
            // Fehlen Schnappschüsse, läuft eine bewegte Entity kurz weiter; sonst steht sie.
            const Sample& last = samples.back();
            if (beyond && last.tick == d.last_tick && samples.size() >= 2) {
                const Sample& before = samples[samples.size() - 2];
                const f64 span = static_cast<f64>(last.tick - before.tick);
                if (span > 0.0 && span <= hold_gap) {
                    const f64 t = (render - static_cast<f64>(last.tick)) / span;
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
                if (render >= a && render < b) {
                    value = blend(samples[i].transform, samples[i + 1].transform,
                                  (render - a) / (b - a));
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

u64 Mirror::local_id(u64 server_id) const noexcept {
    return impl_->local(server_id);
}

usize Mirror::entity_count() const noexcept {
    return impl_->entities.size();
}

u32 Mirror::last_tick() const noexcept {
    return impl_->last_tick;
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
