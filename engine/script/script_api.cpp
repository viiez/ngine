module;

#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.script;

import ngine.core;
import ngine.reflect;
import ngine.serialize;
import ngine.world;
import :script_api;
import :world_api;

namespace ngine::script {

namespace {

struct Context {
    reflect::TypeRegistry* types = nullptr;
    // Ersetzte Typen und ihre Nachfolger, die noch umzuziehen sind.
    std::vector<std::pair<const reflect::TypeInfo*, const reflect::TypeInfo*>> pending;
    std::vector<const reflect::TypeInfo*> registered;
};

Context& context() {
    static Context instance;
    return instance;
}

std::string_view text(const u8* data, std::int32_t length) {
    return {reinterpret_cast<const char*>(data), static_cast<usize>(length < 0 ? 0 : length)};
}

const reflect::TypeInfo* type_at(std::int32_t index) {
    const reflect::TypeRegistry* types = context().types;
    if (types == nullptr || index < 0) {
        return nullptr;
    }
    const auto all = types->types();
    return static_cast<usize>(index) < all.size() ? all[static_cast<usize>(index)] : nullptr;
}

// Gleiche Felder in gleicher Reihenfolge mit gleichen Typen und Hinweisen?
bool same_layout(const reflect::TypeInfo& type,
                 const std::vector<reflect::TypeRegistry::CompositeField>& fields) {
    if (type.fields.size() != fields.size()) {
        return false;
    }
    for (usize i = 0; i < fields.size(); ++i) {
        const reflect::Field& a = type.fields[i];
        const reflect::TypeRegistry::CompositeField& b = fields[i];
        const bool same_range =
            a.range.has_value() == b.range.has_value() &&
            (!a.range || (a.range->min == b.range->min && a.range->max == b.range->max &&
                          a.range->step == b.range->step));
        if (a.name != b.name || a.type != b.type || a.hint != b.hint || !same_range) {
            return false;
        }
    }
    return true;
}

std::int32_t api_register_script(const u8* name, std::int32_t length, const ScriptFieldDesc* fields,
                                 std::int32_t count) {
    Context& c = context();
    if (c.types == nullptr) {
        return -1;
    }
    const std::string type_name{text(name, length)};
    std::vector<reflect::TypeRegistry::CompositeField> composite;
    for (std::int32_t i = 0; i < count; ++i) {
        const ScriptFieldDesc& desc = fields[i];
        const std::string field_type{text(desc.type, desc.type_length)};
        const reflect::TypeInfo* type = c.types->find(field_type);
        if (type == nullptr) {
            log::error("Skript {}: Feldtyp {} gibt es in der Engine nicht", type_name, field_type);
            return -1;
        }
        reflect::TypeRegistry::CompositeField field{
            .name = std::string{text(desc.name, desc.name_length)}, .type = type};
        if (desc.has_range != 0) {
            field.range = reflect::Range{.min = desc.min, .max = desc.max, .step = desc.step};
        }
        if (desc.hint >= 0 && desc.hint <= static_cast<std::int32_t>(reflect::Hint::Asset)) {
            field.hint = static_cast<reflect::Hint>(desc.hint);
        }
        composite.push_back(std::move(field));
    }

    const reflect::TypeInfo* existing = c.types->find(type_name);
    if (existing != nullptr && existing->composite == nullptr) {
        log::error("Skript {} heißt wie eine Komponente der Engine", type_name);
        return -1;
    }
    if (existing != nullptr && same_layout(*existing, composite)) {
        c.registered.push_back(existing);
        return static_cast<std::int32_t>(existing->index);
    }
    if (existing != nullptr) {
        c.types->retire(*existing);
    }
    const reflect::TypeInfo& added = c.types->add_composite(type_name, composite);
    if (existing != nullptr) {
        c.pending.emplace_back(existing, &added);
        log::info("Skript {}: Felder geändert, Komponenten ziehen um", type_name);
    }
    c.registered.push_back(&added);
    return static_cast<std::int32_t>(added.index);
}

std::int64_t api_field_offset(std::int32_t type, std::int32_t field) {
    const reflect::TypeInfo* info = type_at(type);
    if (info == nullptr) {
        return -1;
    }
    if (field < 0) {
        return static_cast<std::int64_t>(info->size);
    }
    return static_cast<usize>(field) < info->fields.size()
               ? static_cast<std::int64_t>(info->fields[static_cast<usize>(field)].offset)
               : -1;
}

void* api_prototype(std::int32_t type) {
    const reflect::TypeInfo* info = type_at(type);
    return info != nullptr && info->composite != nullptr
               ? reflect::TypeRegistry::composite_prototype(*info)
               : nullptr;
}

std::int32_t api_string_get(const void* string, const u8** out) {
    const auto& value = *static_cast<const std::string*>(string);
    *out = reinterpret_cast<const u8*>(value.data());
    return static_cast<std::int32_t>(value.size());
}

void api_string_set(void* string, const u8* data, std::int32_t length) {
    static_cast<std::string*>(string)->assign(text(data, length));
}

} // namespace

ScriptApi make_script_api() {
    return ScriptApi{
        .register_script = api_register_script,
        .field_offset = api_field_offset,
        .prototype = api_prototype,
        .string_get = api_string_get,
        .string_set = api_string_set,
    };
}

void set_script_registry(reflect::TypeRegistry* types) {
    context().types = types;
    context().pending.clear();
    context().registered.clear();
}

reflect::TypeRegistry* script_registry() noexcept {
    return context().types;
}

usize migrate_script_types() {
    Context& c = context();
    world::World* world = script_world();
    usize moved = 0;
    if (world != nullptr) {
        for (const auto& [old_type, new_type] : c.pending) {
            const world::ComponentId from = world->component(*old_type);
            const world::ComponentId to = world->component(*new_type);
            std::vector<u64> entities;
            const world::detail::Term term{.component = from, .size = old_type->size};
            world::detail::QueryBase query{*world, std::span{&term, 1}};
            std::vector<world::detail::Chunk> chunks;
            query.collect(chunks);
            for (const auto& chunk : chunks) {
                entities.insert(entities.end(), chunk.entities, chunk.entities + chunk.count);
            }
            for (const u64 entity : entities) {
                const json::Value values = serialize::to_json(world->get(entity, from), *old_type);
                world->remove(entity, from);
                void* data = world->ensure(entity, to);
                if (auto loaded = serialize::from_json(values, data, *new_type); !loaded) {
                    log::warn("Skript {}: Werte von Entity {} passen nicht mehr: {}",
                              new_type->name, entity, loaded.error());
                }
                world->modified(entity, to);
                ++moved;
            }
        }
    }
    c.pending.clear();
    return moved;
}

const std::vector<const reflect::TypeInfo*>& registered_script_types() noexcept {
    return context().registered;
}

void clear_registered_script_types() {
    context().registered.clear();
}

} // namespace ngine::script
