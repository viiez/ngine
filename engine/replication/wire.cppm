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


// Bausteine der Kodierung, die Schnappschüsse, Server und Client teilen (R5.4, aus snapshot.cpp):
// Varints, Transforms quantisiert, Teile der Komponenten und Einträge eines Schnappschusses. Nichts
// davon ist exportiert; die Einheiten des Moduls sehen es über import :wire.
export module ngine.replication:wire;


import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.serialize;
import ngine.world;
import :schema;
import :snapshot;

namespace ngine::replication::wire {


using serialize::BinaryReader;
using serialize::BinaryWriter;

inline constexpr u8 flag_new = 1;
inline constexpr u8 flag_parent = 2;

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

inline constexpr f64 cell_size = 64.0;
inline constexpr f64 inv_sqrt2 = 0.70710678118654752440;
inline constexpr u32 rotation_bits = 15;
inline constexpr f64 rotation_max = static_cast<f64>((1u << rotation_bits) - 1);

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

// Eine gesendete bzw. empfangene View mit ihrem Tick, Basis späterer Deltas.
struct SentView {
    u32 tick = 0;
    std::shared_ptr<const View> view;
};

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

} // namespace ngine::replication::wire
