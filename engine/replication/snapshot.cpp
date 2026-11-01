module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

module ngine.replication;

import ngine.core;
import ngine.reflect;
import ngine.serialize;
import ngine.world;

namespace ngine::replication {

namespace {

void check_component_count(ComponentList components) {
    verify(components.size() <= 0xFFFF, "Höchstens 65535 replizierte Komponenten");
}

} // namespace

void register_types(reflect::TypeRegistry& types) {
    if (types.find<Replicated>() == nullptr) {
        types.add<Replicated>("Replicated").field("server_id", &Replicated::server_id);
    }
}

std::vector<std::byte> encode_welcome(ComponentList components) {
    check_component_count(components);
    serialize::BinaryWriter writer;
    writer.write(static_cast<u8>(MessageKind::Welcome));
    writer.write(protocol_version);
    writer.write(static_cast<u16>(components.size()));
    for (const reflect::TypeInfo* type : components) {
        writer.write_string(type->name);
    }
    return writer.take();
}

Result<void> check_welcome(std::span<const std::byte> payload, ComponentList components) {
    serialize::BinaryReader reader{payload};
    u32 version = 0;
    u16 count = 0;
    if (!reader.read(version) || !reader.read(count)) {
        return std::unexpected("Willkommensnachricht ist unvollständig");
    }
    if (version != protocol_version) {
        return std::unexpected(std::format("Der Server spricht Protokoll {}, der Client {}",
                                           version, protocol_version));
    }
    if (count != components.size()) {
        return std::unexpected(std::format(
            "Der Server repliziert {} Komponenten, der Client kennt {}", count, components.size()));
    }
    std::string name;
    for (const reflect::TypeInfo* type : components) {
        if (!reader.read_string(name)) {
            return std::unexpected("Willkommensnachricht ist unvollständig");
        }
        if (name != type->name) {
            return std::unexpected(
                std::format("Komponente {} auf dem Server, {} auf dem Client", name, type->name));
        }
    }
    return {};
}

std::vector<std::byte> encode_snapshot(world::World& world, ComponentList components, u32 tick) {
    NGINE_PROFILE_ZONE("replication::encode_snapshot");
    check_component_count(components);
    std::vector<world::ComponentId> ids;
    ids.reserve(components.size());
    for (const reflect::TypeInfo* type : components) {
        ids.push_back(world.component(*type));
    }

    std::vector<u64> entities;
    world.each<const Replicated>([&](world::Entity entity, const Replicated& /*replicated*/) {
        entities.push_back(entity.id());
    });

    serialize::BinaryWriter writer;
    writer.write(static_cast<u8>(MessageKind::Snapshot));
    writer.write(tick);
    writer.write(static_cast<u32>(entities.size()));
    for (const u64 entity : entities) {
        writer.write(entity);
        u16 count = 0;
        for (const world::ComponentId id : ids) {
            count += world.has(entity, id) ? 1 : 0;
        }
        writer.write(count);
        for (usize i = 0; i < ids.size(); ++i) {
            const void* data = world.get(entity, ids[i]);
            if (data == nullptr) {
                continue;
            }
            writer.write(static_cast<u16>(i));
            serialize::write_binary(writer, data, *components[i]);
        }
    }
    return writer.take();
}

Mirror::Mirror(world::World& world, ComponentList components)
    : world_{&world}, components_{components.begin(), components.end()} {
    check_component_count(components);
    component_ids_.reserve(components_.size());
    for (const reflect::TypeInfo* type : components_) {
        component_ids_.push_back(world.component(*type));
    }
}

Result<bool> Mirror::apply(std::span<const std::byte> payload) {
    NGINE_PROFILE_ZONE("replication::Mirror::apply");
    serialize::BinaryReader reader{payload};
    u32 tick = 0;
    u32 count = 0;
    if (!reader.read(tick) || !reader.read(count)) {
        return std::unexpected("Schnappschuss ist unvollständig");
    }
    // Vergleich mit Überlauf: Ticks zählen endlos weiter.
    if (has_tick_ && static_cast<i32>(tick - last_tick_) <= 0) {
        return false;
    }

    world::World& world = *world_;
    std::unordered_set<u64> seen;
    std::vector<bool> present(components_.size());
    for (u32 e = 0; e < count; ++e) {
        u64 server_id = 0;
        u16 component_count = 0;
        if (!reader.read(server_id) || !reader.read(component_count)) {
            return std::unexpected("Schnappschuss ist unvollständig");
        }
        u64& local = entities_[server_id];
        if (local == 0 || !world.alive(local)) {
            local = world.create().set(Replicated{server_id}).id();
        }
        seen.insert(server_id);

        std::ranges::fill(present, false);
        for (u16 c = 0; c < component_count; ++c) {
            u16 index = 0;
            if (!reader.read(index) || index >= components_.size()) {
                return std::unexpected("Schnappschuss nennt eine unbekannte Komponente");
            }
            const world::ComponentId id = component_ids_[index];
            void* data = world.ensure(local, id);
            if (auto result = serialize::read_binary(reader, data, *components_[index]); !result) {
                return std::unexpected(std::format("Schnappschuss, Komponente {}: {}",
                                                   components_[index]->name, result.error()));
            }
            world.modified(local, id);
            present[index] = true;
        }
        for (usize i = 0; i < components_.size(); ++i) {
            if (!present[i] && world.has(local, component_ids_[i])) {
                world.remove(local, component_ids_[i]);
            }
        }
    }
    if (reader.remaining() != 0) {
        return std::unexpected("Schnappschuss hat überzählige Bytes");
    }

    std::erase_if(entities_, [&](const auto& entry) {
        if (seen.contains(entry.first)) {
            return false;
        }
        if (world.alive(entry.second)) {
            world.destroy(entry.second);
        }
        return true;
    });
    last_tick_ = tick;
    has_tick_ = true;
    return true;
}

u64 Mirror::local_id(u64 server_id) const noexcept {
    const auto it = entities_.find(server_id);
    return it == entities_.end() ? 0 : it->second;
}

void Mirror::clear() {
    for (const auto& [server_id, local] : entities_) {
        if (world_->alive(local)) {
            world_->destroy(local);
        }
    }
    entities_.clear();
    has_tick_ = false;
    last_tick_ = 0;
}

} // namespace ngine::replication
