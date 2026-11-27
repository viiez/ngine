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
import :wire;

namespace ngine::replication {

using namespace wire;
using serialize::BinaryReader;
using serialize::BinaryWriter;


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
} // namespace ngine::replication
