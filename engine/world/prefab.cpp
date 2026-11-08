module;

#include <new>

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
import ngine.math;
import ngine.reflect;
import ngine.serialize;
import ngine.filesystem;

namespace ngine::world {

namespace {

// Daten einer Komponente type an entity, nullptr ohne. Über die Besucher-Schnittstelle, damit es
// auch auf einer konstanten Welt geht.
const void* find_component(const World& world, u64 entity, const reflect::TypeInfo* type) {
    if (type == nullptr) {
        return nullptr;
    }
    struct Search {
        const reflect::TypeInfo* type;
        const void* found = nullptr;
    } search{.type = type};
    world.for_each_component(entity, ComponentVisitor{
                                         .context = &search,
                                         .visit =
                                             [](void* context, const reflect::TypeInfo& info,
                                                ComponentId /*component*/, const void* data) {
                                                 auto& s = *static_cast<Search*>(context);
                                                 if (&info == s.type) {
                                                     s.found = data;
                                                 }
                                             },
                                     });
    return search.found;
}

const PrefabInstance* instance_of(const World& world, u64 entity) {
    return static_cast<const PrefabInstance*>(
        find_component(world, entity, world.types().find<PrefabInstance>()));
}

const PrefabNode* node_of(const World& world, u64 entity) {
    return static_cast<const PrefabNode*>(
        find_component(world, entity, world.types().find<PrefabNode>()));
}

// Knoten der Instanz unterhalb von entity (Tiefensuche, Eltern vor Kindern). Eine verschachtelte
// Instanz gehört nicht dazu.
void collect_nodes(const World& world, u64 entity, std::vector<std::pair<u32, u64>>& out) {
    const PrefabNode* node = node_of(world, entity);
    if (node == nullptr) {
        return;
    }
    out.emplace_back(node->node, entity);
    for (const u64 child : world.children(entity)) {
        if (instance_of(world, child) == nullptr) {
            collect_nodes(world, child, out);
        }
    }
}

// Komponente als JSON-Objekt, leer wenn sie fehlt.
const json::Value* member(const json::Value& object, std::string_view key) {
    return object.object() != nullptr ? object.find(key) : nullptr;
}

// Felder von current, die von base abweichen (base nullptr: alle).
json::Value changed_fields(const json::Value& current, const json::Value* base) {
    json::Value result{json::Object{}};
    if (current.object() == nullptr) {
        return current;
    }
    for (const json::Member& field : *current.object()) {
        const json::Value* before = base != nullptr ? member(*base, field.key) : nullptr;
        if (before == nullptr || !(*before == field.value)) {
            result[field.key] = field.value;
        }
    }
    return result;
}

// Wert einer Komponente, wie ihn die Vorlage setzt: Standardwert des Typs mit den Feldern aus
// partial, wieder als JSON. So vergleichen Überschreibungen gleich geschriebene Werte, auch wenn
// die Vorlage Felder auslässt oder Zahlen anders schreibt (6 statt 6.0).
json::Value effective_value(const reflect::TypeInfo& type, const json::Value& partial) {
    void* data = ::operator new(type.size, std::align_val_t{type.alignment});
    type.ops.construct(data);
    json::Value result =
        serialize::from_json(partial, data, type) ? serialize::to_json(data, type) : partial;
    type.ops.destroy(data);
    ::operator delete(data, std::align_val_t{type.alignment});
    return result;
}

// Überschreibungen eines Knotens: Komponente → abweichende Felder.
json::Value node_overrides(const World& world, u64 entity, const Prefab::Node* node) {
    const json::Value current = components_to_json(world, entity);
    json::Value result{json::Object{}};
    for (const json::Member& component : *current.object()) {
        const json::Value* partial =
            node != nullptr ? member(node->components, component.key) : nullptr;
        const reflect::TypeInfo* type = world.types().find(component.key);
        json::Value effective;
        if (partial != nullptr && type != nullptr) {
            effective = effective_value(*type, *partial);
        }
        const json::Value* base = partial != nullptr && type != nullptr ? &effective : partial;
        json::Value fields = changed_fields(component.value, base);
        const bool whole = base == nullptr;
        if (whole || (fields.object() != nullptr && !fields.object()->empty())) {
            result[component.key] = whole ? component.value : std::move(fields);
        }
    }
    return result;
}

// Überschreibungen aus dem Format von prefab_overrides() als Knoten → Komponenten.
std::unordered_map<u32, const json::Value*> overrides_by_node(const json::Value* overrides) {
    std::unordered_map<u32, const json::Value*> result;
    if (overrides == nullptr || overrides->array() == nullptr) {
        return result;
    }
    for (const json::Value& entry : *overrides->array()) {
        const json::Value* node = member(entry, "node");
        const json::Value* components = member(entry, "components");
        const auto id = node != nullptr ? node->to_u64() : std::nullopt;
        if (id && components != nullptr && components->object() != nullptr) {
            result[static_cast<u32>(*id)] = components;
        }
    }
    return result;
}

template <typename T>
void set_value(World& world, u64 entity, const T& value) {
    Entity{world, entity}.set(value);
}

} // namespace

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

Result<void> components_from_json(World& world, u64 entity, const json::Value& components,
                                  std::string_view context) {
    if (components.object() == nullptr) {
        return std::unexpected(std::format("{}: \"components\" ist kein Objekt", context));
    }
    for (const json::Member& member : *components.object()) {
        const reflect::TypeInfo* type = world.types().find(member.key);
        if (type == nullptr || type->kind != reflect::Kind::Struct) {
            log::warn("{}: Komponente '{}' ist unbekannt und wird übersprungen", context,
                      member.key);
            continue;
        }
        const ComponentId component = world.component(*type);
        void* data = world.ensure(entity, component);
        if (auto result = serialize::from_json(member.value, data, *type); !result) {
            return std::unexpected(std::format("{}: {}", context, result.error()));
        }
        world.modified(entity, component);
    }
    return {};
}

Result<Prefab> parse_prefab(std::string_view text) {
    auto parsed = json::parse(text);
    if (!parsed) {
        return std::unexpected(
            std::format("Prefab ist kein gültiges JSON (Zeile {}, Spalte {}): {}",
                        parsed.error().line, parsed.error().column, parsed.error().message));
    }
    const json::Value* format = parsed->find("format");
    if (format == nullptr || format->string() == nullptr || *format->string() != prefab_format) {
        return std::unexpected(
            std::string{"Kein NGine-Prefab (\"format\" fehlt oder passt nicht)"});
    }
    const json::Value* version = parsed->find("version");
    if (version == nullptr || version->to_i64() != prefab_version) {
        return std::unexpected(
            std::format("Prefab-Version wird nicht unterstützt (erwartet {})", prefab_version));
    }
    const json::Value* entities = parsed->find("entities");
    if (entities == nullptr || entities->array() == nullptr || entities->array()->empty()) {
        return std::unexpected(std::string{"Prefab ohne Entities"});
    }
    Prefab prefab;
    std::unordered_map<u64, usize> index;
    for (const json::Value& object : *entities->array()) {
        const json::Value* id_value = object.find("id");
        const auto id = id_value != nullptr ? id_value->to_u64() : std::nullopt;
        if (!id || *id == 0 || *id > 0xFFFFFFFFu || index.contains(*id)) {
            return std::unexpected(
                std::string{"Prefab: Entity ohne gültige oder mit doppelter \"id\""});
        }
        Prefab::Node node{.id = static_cast<u32>(*id), .parent = std::nullopt, .components = {}};
        if (const json::Value* parent = object.find("parent")) {
            const auto parent_id = parent->to_u64();
            const auto it = parent_id ? index.find(*parent_id) : index.end();
            if (it == index.end()) {
                return std::unexpected(std::format(
                    "Prefab, Entity {}: Elternteil fehlt oder steht nach dem Kind", *id));
            }
            node.parent = it->second;
        } else if (!prefab.nodes.empty()) {
            return std::unexpected(std::string{"Prefab hat mehr als eine Wurzel"});
        }
        if (prefab.nodes.empty() && node.parent) {
            return std::unexpected(
                std::string{"Die erste Entity eines Prefabs muss die Wurzel sein"});
        }
        if (const json::Value* components = object.find("components")) {
            if (components->object() == nullptr) {
                return std::unexpected(
                    std::format("Prefab, Entity {}: \"components\" ist kein Objekt", *id));
            }
            node.components = *components;
        }
        index.emplace(*id, prefab.nodes.size());
        prefab.nodes.push_back(std::move(node));
    }
    return prefab;
}

Result<Prefab> load_prefab_file(std::string_view path) {
    auto text = filesystem::read_text(path);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    auto prefab = parse_prefab(*text);
    if (!prefab) {
        return std::unexpected(std::format("{}: {}", path, prefab.error()));
    }
    return prefab;
}

std::string write_prefab(const Prefab& prefab) {
    json::Array entities;
    for (const Prefab::Node& node : prefab.nodes) {
        json::Value object;
        object["id"] = node.id;
        if (node.parent) {
            object["parent"] = prefab.nodes[*node.parent].id;
        }
        object["components"] = node.components;
        entities.push_back(std::move(object));
    }
    json::Value root;
    root["format"] = prefab_format;
    root["version"] = prefab_version;
    root["entities"] = json::Value{std::move(entities)};
    return json::write(root);
}

Prefab make_prefab(const World& world, u64 root) {
    Prefab prefab;
    // Tiefensuche, Eltern vor Kindern; IDs zählen ab 1.
    const auto visit = [&](auto& self, u64 entity, std::optional<usize> parent) -> void {
        Prefab::Node node{.id = static_cast<u32>(prefab.nodes.size() + 1),
                          .parent = parent,
                          .components = components_to_json(world, entity)};
        const usize index = prefab.nodes.size();
        prefab.nodes.push_back(std::move(node));
        for (const u64 child : world.children(entity)) {
            self(self, child, index);
        }
    };
    visit(visit, root, std::nullopt);
    // Die Wurzel in den Ursprung: Wo eine Instanz steht, sagt ihre Überschreibung.
    if (json::Value* transform = prefab.nodes.front().components.object() != nullptr
                                     ? &prefab.nodes.front().components["Transform"]
                                     : nullptr;
        transform != nullptr && transform->object() != nullptr) {
        (*transform)["position"] = serialize::to_json(math::DVec3{0.0, 0.0, 0.0}, world.types());
    }
    return prefab;
}

u64 instance_root(const World& world, u64 entity) {
    while (entity != 0 && world.alive(entity)) {
        if (instance_of(world, entity) != nullptr) {
            return entity;
        }
        if (node_of(world, entity) == nullptr) {
            return 0;
        }
        entity = world.parent(entity);
    }
    return 0;
}

std::vector<std::pair<u32, u64>> instance_nodes(const World& world, u64 root) {
    std::vector<std::pair<u32, u64>> nodes;
    if (instance_of(world, root) == nullptr) {
        return nodes;
    }
    const PrefabNode* node = node_of(world, root);
    nodes.emplace_back(node != nullptr ? node->node : 0, root);
    for (const u64 child : world.children(root)) {
        if (instance_of(world, child) == nullptr) {
            collect_nodes(world, child, nodes);
        }
    }
    return nodes;
}

json::Value prefab_overrides(const World& world, u64 root, const Prefab& prefab) {
    json::Array result;
    for (const auto& [node, entity] : instance_nodes(world, root)) {
        json::Value components = node_overrides(world, entity, prefab.find(node));
        if (components.object()->empty()) {
            continue;
        }
        json::Value entry;
        entry["node"] = node;
        entry["components"] = std::move(components);
        result.push_back(std::move(entry));
    }
    return json::Value{std::move(result)};
}

Result<Entity> instantiate(World& world, const Prefab& prefab, u64 prefab_id,
                           const json::Value* overrides, u64 parent) {
    return instantiate_at(world, prefab, prefab_id, overrides, parent, 0);
}

Result<Entity> instantiate_at(World& world, const Prefab& prefab, u64 prefab_id,
                              const json::Value* overrides, u64 parent, u64 root) {
    if (prefab.nodes.empty()) {
        if (root != 0) {
            world.destroy(root);
        }
        return std::unexpected(std::string{"Prefab ohne Entities"});
    }
    const auto by_node = overrides_by_node(overrides);
    std::vector<u64> created;
    created.reserve(prefab.nodes.size());
    const auto fail = [&](std::string message) -> Result<Entity> {
        for (auto it = created.rbegin(); it != created.rend(); ++it) {
            world.destroy(*it);
        }
        return std::unexpected(std::move(message));
    };
    for (const Prefab::Node& node : prefab.nodes) {
        const u64 entity = created.empty() && root != 0 ? root : world.create().id();
        created.push_back(entity);
        const u64 node_parent = node.parent ? created[*node.parent] : parent;
        if (node_parent != 0) {
            world.set_parent(entity, node_parent);
        }
        const std::string context = std::format("Prefab {}, Knoten {}", prefab_id, node.id);
        if (auto result = components_from_json(world, entity, node.components, context); !result) {
            return fail(result.error());
        }
        if (const auto it = by_node.find(node.id); it != by_node.end()) {
            if (auto result = components_from_json(world, entity, *it->second, context); !result) {
                return fail(result.error());
            }
        }
        set_value(world, entity, PrefabNode{.node = node.id});
    }
    set_value(world, created.front(), PrefabInstance{.prefab = prefab_id});
    return Entity{world, created.front()};
}

usize update_instances(World& world, u64 prefab_id, const Prefab& before, const Prefab& after) {
    if (after.nodes.empty()) {
        return 0;
    }
    std::vector<u64> roots;
    for (const u64 entity : world.entity_ids()) {
        if (const PrefabInstance* instance = instance_of(world, entity);
            instance != nullptr && instance->prefab == prefab_id) {
            roots.push_back(entity);
        }
    }
    for (const u64 root : roots) {
        // Erst die Abweichungen vom alten Stand festhalten, dann auf den neuen Stand bringen.
        const json::Value overrides = prefab_overrides(world, root, before);
        const auto by_node = overrides_by_node(&overrides);
        std::unordered_map<u32, u64> entities;
        std::vector<u64> old_entities;
        for (const auto& [node, entity] : instance_nodes(world, root)) {
            if (entity != root) {
                entities.emplace(node, entity);
            }
            old_entities.push_back(entity);
        }
        // Die Wurzel bleibt dieselbe Entity, auch wenn sich ihre ID in der Vorlage ändert.
        entities[after.nodes.front().id] = root;
        std::vector<u64> current(after.nodes.size(), 0);
        for (usize i = 0; i < after.nodes.size(); ++i) {
            const Prefab::Node& node = after.nodes[i];
            const u64 parent = node.parent ? current[*node.parent] : world.parent(root);
            u64 entity = 0;
            if (const auto it = entities.find(node.id);
                it != entities.end() && world.alive(it->second)) {
                entity = it->second;
                entities.erase(it);
                if (i != 0 && world.parent(entity) != parent) {
                    world.set_parent(entity, parent);
                }
            } else {
                entity = world.create().id();
                if (parent != 0) {
                    world.set_parent(entity, parent);
                }
                set_value(world, entity, PrefabNode{.node = node.id});
            }
            current[i] = entity;
            const std::string context = std::format("Prefab {}, Knoten {}", prefab_id, node.id);
            const auto override = by_node.find(node.id);
            // Komponenten, die die alte Vorlage hatte und die neue nicht mehr, fallen weg, wenn
            // die Instanz sie nicht überschreibt.
            if (const Prefab::Node* old = before.find(node.id);
                old != nullptr && old->components.object() != nullptr) {
                for (const json::Member& component : *old->components.object()) {
                    const bool kept = member(node.components, component.key) != nullptr ||
                                      (override != by_node.end() &&
                                       member(*override->second, component.key) != nullptr);
                    const reflect::TypeInfo* type = world.types().find(component.key);
                    if (!kept && type != nullptr) {
                        world.remove(entity, world.component(*type));
                    }
                }
            }
            if (auto result = components_from_json(world, entity, node.components, context);
                !result) {
                log::error("{}", result.error());
            }
            if (override != by_node.end()) {
                if (auto result = components_from_json(world, entity, *override->second, context);
                    !result) {
                    log::error("{}", result.error());
                }
            }
            if (node_of(world, entity)->node != node.id) {
                set_value(world, entity, PrefabNode{.node = node.id});
            }
        }
        // Knoten, die die Vorlage nicht mehr hat (mit allem, was darunter hängt).
        for (const u64 entity : old_entities) {
            if (std::ranges::find(current, entity) == current.end() && world.alive(entity)) {
                world.destroy(entity);
            }
        }
    }
    return roots.size();
}

void unpack_instance(World& world, u64 root) {
    const auto nodes = instance_nodes(world, root);
    const ComponentId node_component = world.component<PrefabNode>();
    for (const auto& [node, entity] : nodes) {
        world.remove(entity, node_component);
    }
    world.remove(root, world.component<PrefabInstance>());
    world.remove(root, world.component<PrefabOverrides>());
}

void register_prefab_types(reflect::TypeRegistry& types) {
    if (types.find<PrefabInstance>() == nullptr) {
        types.add<PrefabInstance>("PrefabInstance")
            .transient()
            .field("prefab", &PrefabInstance::prefab)
            .hint(reflect::Hint::Asset);
    }
    if (types.find<PrefabNode>() == nullptr) {
        types.add<PrefabNode>("PrefabNode").transient().field("node", &PrefabNode::node);
    }
    if (types.find<PrefabOverrides>() == nullptr) {
        types.add<PrefabOverrides>("PrefabOverrides")
            .transient()
            .field("json", &PrefabOverrides::json);
    }
}

} // namespace ngine::world
