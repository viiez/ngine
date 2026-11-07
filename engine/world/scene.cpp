module;

#include <algorithm>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

module ngine.world;

import ngine.core;
import ngine.reflect;
import ngine.serialize;
import ngine.filesystem;

namespace ngine::world {

namespace {

// Ordnet Entities so, dass Eltern vor ihren Kindern stehen: Wurzeln aufsteigend, darunter jeweils
// die Kinder (Tiefensuche).
void append_subtree(const World& world, u64 entity, std::vector<u64>& out) {
    out.push_back(entity);
    for (const u64 child : world.children(entity)) {
        append_subtree(world, child, out);
    }
}

json::Value components_to_json(const World& world, u64 entity) {
    // Reihenfolge der Registrierung statt der Anmeldung in der Welt: Welten, die Komponenten in
    // anderer Reihenfolge angemeldet haben, schreiben so denselben Text.
    std::vector<std::pair<const reflect::TypeInfo*, const void*>> found;
    auto visit = [&](const reflect::TypeInfo& type, ComponentId /*component*/, const void* data) {
        if (!type.transient) {
            found.emplace_back(&type, data);
        }
    };
    world.for_each_component(entity, ComponentVisitor{
                                         .context = &visit,
                                         .visit =
                                             [](void* context, const reflect::TypeInfo& type,
                                                ComponentId component, const void* data) {
                                                 (*static_cast<decltype(visit)*>(context))(
                                                     type, component, data);
                                             },
                                     });
    std::ranges::sort(found, {}, [](const auto& entry) { return entry.first->index; });
    json::Value components{json::Object{}};
    for (const auto& [type, data] : found) {
        components[type->name] = serialize::to_json(data, *type);
    }
    return components;
}

std::string entity_error(i64 file_id, std::string_view message) {
    return std::format("Szene, Entity {}: {}", file_id, message);
}

} // namespace

std::string save_scene(const World& world) {
    std::vector<u64> ordered;
    for (const u64 entity : world.entity_ids()) {
        if (world.parent(entity) == 0) {
            append_subtree(world, entity, ordered);
        }
    }

    std::unordered_map<u64, i64> file_ids;
    json::Array entities;
    entities.reserve(ordered.size());
    for (const u64 entity : ordered) {
        const i64 file_id = static_cast<i64>(file_ids.size()) + 1;
        file_ids.emplace(entity, file_id);

        json::Value object;
        object["id"] = file_id;
        if (const u64 parent = world.parent(entity); parent != 0) {
            object["parent"] = file_ids.at(parent);
        }
        object["components"] = components_to_json(world, entity);
        entities.push_back(std::move(object));
    }

    json::Value scene;
    scene["format"] = scene_format;
    scene["version"] = scene_version;
    scene["entities"] = json::Value{std::move(entities)};
    return json::write(scene);
}

Result<usize> load_scene(World& world, std::string_view text) {
    auto parsed = json::parse(text);
    if (!parsed) {
        return std::unexpected(std::format("Szene ist kein gültiges JSON (Zeile {}, Spalte {}): {}",
                                           parsed.error().line, parsed.error().column,
                                           parsed.error().message));
    }
    const json::Value& scene = *parsed;
    const json::Value* format = scene.find("format");
    if (format == nullptr || format->string() == nullptr || *format->string() != scene_format) {
        return std::unexpected(
            std::string{"Keine NGine-Szene (\"format\" fehlt oder passt nicht)"});
    }
    const json::Value* version = scene.find("version");
    if (version == nullptr || version->to_i64() != scene_version) {
        return std::unexpected(
            std::format("Szenenversion wird nicht unterstützt (erwartet {})", scene_version));
    }
    const json::Value* entities = scene.find("entities");
    if (entities == nullptr || entities->array() == nullptr) {
        return std::unexpected(std::string{"Szene ohne \"entities\"-Array"});
    }

    // Alle Entities vorab anlegen und aufsteigend vergeben: Die Welt verwendet Kennungen gelöschter
    // Entities in umgekehrter Reihenfolge wieder, die Datei soll aber in ihrer Reihenfolge
    // ankommen, damit sie ohne Änderung textgleich gespeichert wird.
    std::vector<u64> fresh;
    fresh.reserve(entities->array()->size());
    for (usize i = 0; i < entities->array()->size(); ++i) {
        fresh.push_back(world.create().id());
    }
    std::ranges::sort(fresh);
    usize next = 0;
    // Bei einem Fehler: nicht benutzte Entities wieder weg, das bisher Geladene bleibt.
    const auto fail = [&](std::string message) -> Result<usize> {
        for (usize i = next; i < fresh.size(); ++i) {
            world.destroy(fresh[i]);
        }
        return std::unexpected(std::move(message));
    };

    std::unordered_map<i64, Entity> created;
    for (const json::Value& object : *entities->array()) {
        const json::Value* id_value = object.find("id");
        const auto file_id = id_value != nullptr ? id_value->to_i64() : std::nullopt;
        if (!file_id || created.contains(*file_id)) {
            return fail(std::string{"Szene: Entity ohne gültige oder mit doppelter \"id\""});
        }

        Entity entity = world.entity(fresh[next++]);
        created.emplace(*file_id, entity);

        if (const json::Value* parent = object.find("parent")) {
            const auto parent_id = parent->to_i64();
            const auto it = parent_id ? created.find(*parent_id) : created.end();
            if (it == created.end()) {
                return fail(entity_error(*file_id, "Elternteil fehlt oder steht nach dem Kind"));
            }
            entity.set_parent(it->second);
        }

        const json::Value* components = object.find("components");
        if (components == nullptr) {
            continue;
        }
        if (components->object() == nullptr) {
            return fail(entity_error(*file_id, "\"components\" ist kein Objekt"));
        }
        for (const json::Member& member : *components->object()) {
            const reflect::TypeInfo* type = world.types().find(member.key);
            if (type == nullptr || type->kind != reflect::Kind::Struct) {
                log::warn("Szene, Entity {}: Komponente '{}' ist unbekannt und wird übersprungen",
                          *file_id, member.key);
                continue;
            }
            const ComponentId component = world.component(*type);
            void* data = world.ensure(entity.id(), component);
            if (auto result = serialize::from_json(member.value, data, *type); !result) {
                return fail(entity_error(*file_id, result.error()));
            }
            world.modified(entity.id(), component);
        }
    }
    return created.size();
}

Result<void> save_scene_file(const World& world, std::string_view path) {
    return filesystem::write_text(path, save_scene(world));
}

Result<usize> load_scene_file(World& world, std::string_view path) {
    auto text = filesystem::read_text(path);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    return load_scene(world, *text);
}

} // namespace ngine::world
