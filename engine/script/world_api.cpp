module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module ngine.script;

import ngine.core;
import ngine.reflect;
import ngine.world;
import :world_api;

namespace ngine::script {

namespace {

// Eine Abfrage aus C#, zwischengespeichert nach ihren Typen.
struct CachedQuery {
    world::detail::QueryBase query;
    std::vector<world::detail::Chunk> chunks;
    std::vector<NativeChunk> native;
};

struct Context {
    world::World* world = nullptr;
    std::optional<world::Commands> commands;
    // Komponente pro Index in der Registry; 0 heißt noch nicht nachgeschlagen.
    std::vector<world::ComponentId> components;
    std::map<std::vector<std::int32_t>, std::unique_ptr<CachedQuery>> queries;
};

Context& context() {
    static Context instance;
    return instance;
}

std::string_view text(const u8* data, std::int32_t length) {
    return {reinterpret_cast<const char*>(data), static_cast<usize>(length < 0 ? 0 : length)};
}

const reflect::TypeInfo* type_at(std::int32_t index) {
    const Context& c = context();
    if (c.world == nullptr || index < 0) {
        return nullptr;
    }
    const auto types = c.world->types().types();
    return static_cast<usize>(index) < types.size() ? types[static_cast<usize>(index)] : nullptr;
}

world::ComponentId component_at(std::int32_t index) {
    Context& c = context();
    const reflect::TypeInfo* type = type_at(index);
    if (type == nullptr || type->kind != reflect::Kind::Struct) {
        return 0;
    }
    if (c.components.size() <= static_cast<usize>(index)) {
        c.components.resize(static_cast<usize>(index) + 1, 0);
    }
    world::ComponentId& id = c.components[static_cast<usize>(index)];
    if (id == 0) {
        id = c.world->component(*type);
    }
    return id;
}

// Lebt die Entity, und ist sie keine vorläufige aus dem Befehlspuffer?
bool live(u64 entity) {
    const Context& c = context();
    return c.world != nullptr && entity != 0 && !world::Commands::is_pending(entity) &&
           c.world->alive(entity);
}

// Ob die Entity Ziel eines Befehls sein kann (lebt oder ist im Puffer angelegt).
bool addressable(u64 entity) {
    const Context& c = context();
    return c.world != nullptr && entity != 0 &&
           (world::Commands::is_pending(entity) || c.world->alive(entity));
}

// Kopiert die Felder, die C# sieht (alles außer Texten), von source nach target.
void copy_visible(const reflect::TypeInfo& type, std::byte* target, const std::byte* source) {
    if (type.trivial) {
        std::memcpy(target, source, type.size);
        return;
    }
    for (const reflect::Field& field : type.fields) {
        if (!csharp_visible(field)) {
            continue;
        }
        copy_visible(*field.type, target + field.offset, source + field.offset);
    }
}

// --- Funktionen für C# ---------------------------------------------------------------------

std::int32_t api_type_index(const u8* name, std::int32_t length) {
    const Context& c = context();
    if (c.world == nullptr) {
        return -1;
    }
    const reflect::TypeInfo* type = c.world->types().find(text(name, length));
    return type != nullptr ? static_cast<std::int32_t>(type->index) : -1;
}

void* api_get(u64 entity, std::int32_t type, std::int32_t write) {
    if (!live(entity)) {
        return nullptr;
    }
    const world::ComponentId component = component_at(type);
    if (component == 0 || !context().world->has(entity, component)) {
        return nullptr;
    }
    world::World& w = *context().world;
    if (write == 0) {
        return const_cast<void*>(w.get(entity, component));
    }
    void* data = w.get_mut(entity, component);
    w.modified(entity, component);
    return data;
}

std::int32_t api_has(u64 entity, std::int32_t type) {
    if (!live(entity)) {
        return 0;
    }
    const world::ComponentId component = component_at(type);
    return component != 0 && context().world->has(entity, component) ? 1 : 0;
}

std::int32_t api_set(u64 entity, std::int32_t type, const void* value) {
    Context& c = context();
    const reflect::TypeInfo* info = type_at(type);
    const world::ComponentId component = component_at(type);
    if (info == nullptr || component == 0 || value == nullptr || !addressable(entity)) {
        return -1;
    }
    const auto* source = static_cast<const std::byte*>(value);
    if (live(entity) && c.world->has(entity, component)) {
        copy_visible(*info, static_cast<std::byte*>(c.world->get_mut(entity, component)), source);
        c.world->modified(entity, component);
        return 0;
    }
    // Neu an der Entity: über den Befehlspuffer, damit Referenzen aus Get gültig bleiben.
    if (info->trivial) {
        c.commands->set(c.world->entity(entity), *info, value);
        return 0;
    }
    // Typen mit Texten: Standardwert, darauf die Felder aus C#.
    auto storage = std::make_unique<std::byte[]>(info->size + info->alignment);
    void* raw = storage.get();
    usize space = info->size + info->alignment;
    void* aligned = std::align(info->alignment, info->size, raw, space);
    info->construct(aligned);
    copy_visible(*info, static_cast<std::byte*>(aligned), source);
    c.commands->set(c.world->entity(entity), *info, aligned);
    info->destroy(aligned);
    return 0;
}

std::int32_t api_remove(u64 entity, std::int32_t type) {
    Context& c = context();
    const reflect::TypeInfo* info = type_at(type);
    if (info == nullptr || component_at(type) == 0 || !addressable(entity)) {
        return -1;
    }
    c.commands->remove(c.world->entity(entity), *info);
    return 0;
}

u64 api_create(const u8* name, std::int32_t length) {
    Context& c = context();
    if (c.world == nullptr) {
        return 0;
    }
    return length > 0 ? c.commands->create(text(name, length)).id() : c.commands->create().id();
}

std::int32_t api_destroy(u64 entity) {
    Context& c = context();
    if (!addressable(entity)) {
        return -1;
    }
    c.commands->destroy(c.world->entity(entity));
    return 0;
}

std::int32_t api_alive(u64 entity) {
    return live(entity) ? 1 : 0;
}

u64 api_parent(u64 entity) {
    return live(entity) ? context().world->parent(entity) : 0;
}

std::int32_t api_set_parent(u64 child, u64 parent) {
    Context& c = context();
    if (!addressable(child) || (parent != 0 && !addressable(parent))) {
        return -1;
    }
    c.commands->set_parent(c.world->entity(child),
                           parent != 0 ? c.world->entity(parent) : world::Entity{});
    return 0;
}

std::int32_t api_children(u64 entity, u64* out, std::int32_t capacity) {
    if (!live(entity)) {
        return 0;
    }
    const std::vector<u64> children = context().world->children(entity);
    const usize count = std::min(children.size(), static_cast<usize>(capacity < 0 ? 0 : capacity));
    if (out != nullptr) {
        std::memcpy(out, children.data(), count * sizeof(u64));
    }
    return static_cast<std::int32_t>(children.size());
}

std::int32_t api_name(u64 entity, const u8** out) {
    if (!live(entity)) {
        return -1;
    }
    const auto* name = context().world->entity(entity).get<world::Name>();
    if (name == nullptr) {
        return -1;
    }
    *out = reinterpret_cast<const u8*>(name->value.data());
    return static_cast<std::int32_t>(name->value.size());
}

std::int32_t api_set_name(u64 entity, const u8* name, std::int32_t length) {
    Context& c = context();
    if (!addressable(entity)) {
        return -1;
    }
    c.commands->set(c.world->entity(entity), world::Name{std::string{text(name, length)}});
    return 0;
}

u64 api_find(const u8* name, std::int32_t length) {
    Context& c = context();
    if (c.world == nullptr) {
        return 0;
    }
    const std::string_view wanted = text(name, length);
    u64 found = 0;
    c.world->each<const world::Name>([&](world::Entity entity, const world::Name& n) {
        if (n.value == wanted && (found == 0 || entity.id() < found)) {
            found = entity.id();
        }
    });
    return found;
}

std::int32_t api_query(const std::int32_t* types, std::int32_t count, const NativeChunk** out) {
    Context& c = context();
    if (c.world == nullptr || count <= 0 || static_cast<usize>(count) > world::max_query_terms) {
        return -1;
    }
    std::vector<std::int32_t> key(types, types + count);
    auto& cached = c.queries[key];
    if (!cached) {
        std::vector<world::detail::Term> terms;
        for (const std::int32_t type : key) {
            const world::ComponentId component = component_at(type);
            if (component == 0) {
                c.queries.erase(key);
                return -1;
            }
            terms.push_back({.component = component, .size = type_at(type)->size});
        }
        cached = std::make_unique<CachedQuery>(
            CachedQuery{.query = world::detail::QueryBase{*c.world, terms}});
    }
    cached->chunks.clear();
    cached->query.collect(cached->chunks);
    cached->native.resize(cached->chunks.size());
    for (usize i = 0; i < cached->chunks.size(); ++i) {
        const world::detail::Chunk& chunk = cached->chunks[i];
        NativeChunk& native = cached->native[i];
        native = {.count = chunk.count, .entities = chunk.entities};
        for (usize term = 0; term < world::max_query_terms; ++term) {
            native.columns[term] = chunk.columns[term];
        }
    }
    *out = cached->native.data();
    return static_cast<std::int32_t>(cached->native.size());
}

} // namespace

WorldApi make_world_api() {
    return WorldApi{
        .type_index = api_type_index,
        .get = api_get,
        .has = api_has,
        .set = api_set,
        .remove = api_remove,
        .create = api_create,
        .destroy = api_destroy,
        .alive = api_alive,
        .parent = api_parent,
        .set_parent = api_set_parent,
        .children = api_children,
        .name = api_name,
        .set_name = api_set_name,
        .find = api_find,
        .query = api_query,
    };
}

void bind_script_world(world::World* world) {
    Context& c = context();
    if (c.commands && !c.commands->empty()) {
        log::warn("Skripte: {} Befehle verworfen, weil die Welt wechselt", c.commands->size());
    }
    c.queries.clear();
    c.components.clear();
    c.commands.reset();
    c.world = world;
    if (world != nullptr) {
        c.commands.emplace(*world);
    }
}

world::World* script_world() noexcept {
    return context().world;
}

void apply_script_commands() {
    Context& c = context();
    if (c.commands) {
        c.commands->apply();
    }
}

} // namespace ngine::script
