module;

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <span>
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

// --- Aufbau einer Szene, unabhängig vom Format ---------------------------------------------

// Ein Eintrag der Datei: eine Entity oder eine ganze Prefab-Instanz.
struct Entry {
    i64 id = 0;
    i64 parent = 0;
    // Knoten der Instanz parent, unter dem der Eintrag hängt (0: an parent selbst).
    u32 parent_node = 0;
    u64 entity = 0;
    // Instanz: Kennung der Vorlage und Überschreibungen (JSON-Array).
    u64 prefab = 0;
    json::Value overrides;
};

// Ordnet Entities so, dass Eltern vor ihren Kindern stehen: Wurzeln aufsteigend, darunter jeweils
// die Kinder (Tiefensuche). Eine Prefab-Instanz wird ein Eintrag; ihre Knoten stehen nicht in der
// Datei, wohl aber Entities, die jemand darunter gehängt hat.
class SceneWriter {
public:
    SceneWriter(const World& world, const PrefabLookup& prefabs)
        : world_{world}, prefabs_{prefabs}, instance_type_{world.types().find<PrefabInstance>()},
          node_type_{world.types().find<PrefabNode>()},
          overrides_type_{world.types().find<PrefabOverrides>()} {}

    std::vector<Entry> build() {
        for (const u64 entity : world_.entity_ids()) {
            if (world_.parent(entity) == 0) {
                visit(entity, 0, 0, 0);
            }
        }
        return std::move(entries_);
    }

private:
    template <typename T>
    const T* get(u64 entity, const reflect::TypeInfo* type) const {
        if (type == nullptr) {
            return nullptr;
        }
        struct Search {
            const reflect::TypeInfo* type;
            const void* found = nullptr;
        } search{.type = type};
        world_.for_each_component(entity, ComponentVisitor{
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
        return static_cast<const T*>(search.found);
    }

    // instance: Eintrag der Instanz, in der entity als Knoten liegt (0: keine).
    void visit(u64 entity, i64 parent, u32 parent_node, i64 instance) {
        if (const auto* prefab = get<PrefabInstance>(entity, instance_type_)) {
            Entry entry{.id = next_id(),
                        .parent = parent,
                        .parent_node = parent_node,
                        .entity = entity,
                        .prefab = prefab->prefab,
                        .overrides = {}};
            if (const Prefab* found = prefabs_ ? prefabs_(prefab->prefab) : nullptr) {
                entry.overrides = prefab_overrides(world_, entity, *found);
            } else if (const auto* stored = get<PrefabOverrides>(entity, overrides_type_);
                       stored != nullptr && !stored->json.empty()) {
                // Vorlage fehlt: die Überschreibungen von damals unverändert zurück.
                auto parsed = json::parse(stored->json);
                entry.overrides = parsed ? *std::move(parsed) : json::Value{json::Array{}};
            } else {
                entry.overrides = json::Value{json::Array{}};
            }
            const i64 id = entry.id;
            entries_.push_back(std::move(entry));
            visit_instance_children(entity, id);
            return;
        }
        if (instance != 0) {
            if (const auto* node = get<PrefabNode>(entity, node_type_)) {
                // Knoten der Instanz: steckt in ihrem Eintrag, nur Fremdes darunter zählt.
                for (const u64 child : world_.children(entity)) {
                    visit_under_node(child, instance, node->node);
                }
                return;
            }
        }
        const i64 id = next_id();
        entries_.push_back(Entry{.id = id,
                                 .parent = parent,
                                 .parent_node = parent_node,
                                 .entity = entity,
                                 .prefab = 0,
                                 .overrides = {}});
        for (const u64 child : world_.children(entity)) {
            visit(child, id, 0, 0);
        }
    }

    void visit_instance_children(u64 root, i64 instance) {
        const auto* node = get<PrefabNode>(root, node_type_);
        for (const u64 child : world_.children(root)) {
            visit_under_node(child, instance, node != nullptr ? node->node : 0);
        }
    }

    void visit_under_node(u64 child, i64 instance, u32 node) {
        if (get<PrefabInstance>(child, instance_type_) == nullptr &&
            get<PrefabNode>(child, node_type_) != nullptr) {
            visit(child, instance, node, instance);
        } else {
            visit(child, instance, node, 0);
        }
    }

    i64 next_id() {
        return ++last_id_;
    }

    const World& world_;
    const PrefabLookup& prefabs_;
    const reflect::TypeInfo* instance_type_;
    const reflect::TypeInfo* node_type_;
    const reflect::TypeInfo* overrides_type_;
    std::vector<Entry> entries_;
    i64 last_id_ = 0;
};

// --- Laden ---------------------------------------------------------------------------------

// Was ein Eintrag beim Laden mitbringt; Komponenten je nach Format als JSON oder Binärdaten.
struct LoadEntry {
    i64 id = 0;
    std::optional<i64> parent;
    u32 parent_node = 0;
    u64 prefab = 0;
    const json::Value* overrides = nullptr;
    const json::Value* components = nullptr;
    std::span<const std::byte> binary;
};

std::string entity_error(i64 file_id, std::string_view message) {
    return std::format("Szene, Entity {}: {}", file_id, message);
}

// Legt die Einträge an. read_binary liest Komponenten aus dem Binärformat (nur dort gesetzt).
template <typename ReadBinary>
Result<usize> create_entries(World& world, std::span<const LoadEntry> entries,
                             const PrefabLookup& prefabs, ReadBinary&& read_binary) {
    // Eine Entity pro Eintrag vorab anlegen und aufsteigend vergeben: Die Welt verwendet
    // Kennungen gelöschter Entities in umgekehrter Reihenfolge wieder, die Datei soll aber in
    // ihrer Reihenfolge ankommen, damit sie ohne Änderung textgleich gespeichert wird.
    // Instanzen bekommen so ihre Wurzel.
    std::vector<u64> fresh;
    fresh.reserve(entries.size());
    for (usize i = 0; i < entries.size(); ++i) {
        fresh.push_back(world.create().id());
    }
    std::ranges::sort(fresh);
    usize next = 0;
    std::vector<u64> instances;
    // Bei einem Fehler: nicht benutzte Entities wieder weg, das bisher Geladene bleibt.
    const auto fail = [&](std::string message) -> Result<usize> {
        for (usize i = next; i < fresh.size(); ++i) {
            world.destroy(fresh[i]);
        }
        return std::unexpected(std::move(message));
    };

    struct Created {
        u64 entity = 0;
        // Knoten → Entity, nur bei Instanzen.
        std::vector<std::pair<u32, u64>> nodes;
    };
    std::unordered_map<i64, Created> created;
    for (const LoadEntry& entry : entries) {
        if (created.contains(entry.id)) {
            return fail(std::string{"Szene: Entity ohne gültige oder mit doppelter \"id\""});
        }
        u64 parent = 0;
        if (entry.parent) {
            const auto it = created.find(*entry.parent);
            if (it == created.end()) {
                return fail(entity_error(entry.id, "Elternteil fehlt oder steht nach dem Kind"));
            }
            parent = it->second.entity;
            if (entry.parent_node != 0) {
                const auto node = std::ranges::find(it->second.nodes, entry.parent_node,
                                                    &std::pair<u32, u64>::first);
                if (node != it->second.nodes.end()) {
                    parent = node->second;
                } else {
                    log::warn("Szene, Entity {}: Knoten {} der Instanz fehlt, sie hängt an deren "
                              "Wurzel",
                              entry.id, entry.parent_node);
                }
            }
        }

        if (entry.prefab != 0) {
            const Prefab* prefab = prefabs ? prefabs(entry.prefab) : nullptr;
            if (prefab != nullptr) {
                auto instance = instantiate_at(world, *prefab, entry.prefab, entry.overrides,
                                               parent, fresh[next++]);
                if (!instance) {
                    return fail(entity_error(entry.id, instance.error()));
                }
                created.emplace(entry.id, Created{.entity = instance->id(),
                                                  .nodes = instance_nodes(world, instance->id())});
                continue;
            }
            // Vorlage fehlt: nur die Wurzel mit der Kennung und den Überschreibungen als Text,
            // damit Speichern nichts verliert.
            log::error("Szene, Entity {}: Prefab {} gibt es nicht", entry.id, entry.prefab);
            const u64 entity = fresh[next++];
            if (parent != 0) {
                world.set_parent(entity, parent);
            }
            Entity{world, entity}.set(PrefabInstance{.prefab = entry.prefab});
            Entity{world, entity}.set(PrefabNode{.node = 1});
            if (entry.overrides != nullptr) {
                Entity{world, entity}.set(PrefabOverrides{.json = json::write(*entry.overrides)});
            }
            created.emplace(entry.id, Created{.entity = entity, .nodes = {}});
            continue;
        }

        const u64 entity = fresh[next++];
        created.emplace(entry.id, Created{.entity = entity, .nodes = {}});
        if (parent != 0) {
            world.set_parent(entity, parent);
        }
        if (entry.components != nullptr) {
            if (auto result = components_from_json(world, entity, *entry.components,
                                                   std::format("Szene, Entity {}", entry.id));
                !result) {
                return fail(result.error());
            }
        } else if (auto result = read_binary(entity, entry); !result) {
            return fail(entity_error(entry.id, result.error()));
        }
    }
    return created.size();
}

// --- Binärformat ---------------------------------------------------------------------------

constexpr u32 binary_magic = 0x4353474Eu; // „NGSC“

// Prüfsumme über den Aufbau eines Typs (Namen und Arten der Felder, rekursiv): Eine Binärszene
// passt nur zu einem Build mit demselben Aufbau.
u64 layout_hash(const reflect::TypeInfo& type, u64 hash = 14695981039346656037ull) {
    const auto mix = [&](std::string_view text) {
        for (const char c : text) {
            hash = (hash ^ static_cast<u8>(c)) * 1099511628211ull;
        }
        hash = (hash ^ 0xFFu) * 1099511628211ull;
    };
    mix(type.name);
    hash = (hash ^ static_cast<u64>(type.kind)) * 1099511628211ull;
    hash = (hash ^ type.size) * 1099511628211ull;
    for (const reflect::Field& field : type.fields) {
        mix(field.name);
        hash = layout_hash(*field.type, hash);
    }
    return hash;
}

} // namespace

std::string save_scene(const World& world, const PrefabLookup& prefabs) {
    std::vector<Entry> entries = SceneWriter{world, prefabs}.build();
    json::Array array;
    array.reserve(entries.size());
    for (Entry& entry : entries) {
        json::Value object;
        object["id"] = entry.id;
        if (entry.parent != 0) {
            object["parent"] = entry.parent;
            if (entry.parent_node != 0) {
                object["parent_node"] = entry.parent_node;
            }
        }
        if (entry.prefab != 0) {
            object["prefab"] = entry.prefab;
            if (entry.overrides.array() != nullptr && !entry.overrides.array()->empty()) {
                object["overrides"] = std::move(entry.overrides);
            }
        } else {
            object["components"] = components_to_json(world, entry.entity);
        }
        array.push_back(std::move(object));
    }

    json::Value scene;
    scene["format"] = scene_format;
    scene["version"] = scene_version;
    scene["entities"] = json::Value{std::move(array)};
    return json::write(scene);
}

Result<usize> load_scene(World& world, std::string_view text, const PrefabLookup& prefabs) {
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

    static const json::Value no_components{json::Object{}};
    std::vector<LoadEntry> entries;
    entries.reserve(entities->array()->size());
    for (const json::Value& object : *entities->array()) {
        const json::Value* id_value = object.find("id");
        const auto file_id = id_value != nullptr ? id_value->to_i64() : std::nullopt;
        if (!file_id) {
            return std::unexpected(
                std::string{"Szene: Entity ohne gültige oder mit doppelter \"id\""});
        }
        LoadEntry entry{.id = *file_id};
        if (const json::Value* parent = object.find("parent")) {
            entry.parent = parent->to_i64().value_or(-1);
            if (const json::Value* node = object.find("parent_node")) {
                entry.parent_node = static_cast<u32>(node->to_u64().value_or(0));
            }
        }
        if (const json::Value* prefab = object.find("prefab")) {
            entry.prefab = prefab->to_u64().value_or(0);
            if (entry.prefab == 0) {
                return std::unexpected(entity_error(*file_id, "\"prefab\" ist keine Kennung"));
            }
            entry.overrides = object.find("overrides");
        } else {
            const json::Value* components = object.find("components");
            entry.components = components != nullptr ? components : &no_components;
        }
        entries.push_back(entry);
    }
    return create_entries(world, entries, prefabs,
                          [](u64, const LoadEntry&) -> Result<void> { return {}; });
}

std::vector<std::byte> save_scene_binary(const World& world, const PrefabLookup& prefabs) {
    std::vector<Entry> entries = SceneWriter{world, prefabs}.build();
    // Typen, die vorkommen, mit Name und Prüfsumme des Aufbaus; Komponenten verweisen per Index.
    std::vector<const reflect::TypeInfo*> types;
    std::unordered_map<const reflect::TypeInfo*, u32> type_index;
    struct Component {
        u32 type = 0;
        const void* data = nullptr;
    };
    std::vector<std::vector<Component>> components(entries.size());
    for (usize i = 0; i < entries.size(); ++i) {
        if (entries[i].prefab != 0) {
            continue;
        }
        std::vector<std::pair<const reflect::TypeInfo*, const void*>> found;
        auto visit = [&](const reflect::TypeInfo& type, ComponentId, const void* data) {
            if (!type.transient) {
                found.emplace_back(&type, data);
            }
        };
        world.for_each_component(
            entries[i].entity,
            ComponentVisitor{.context = &visit,
                             .visit = [](void* context, const reflect::TypeInfo& type,
                                         ComponentId component, const void* data) {
                                 (*static_cast<decltype(visit)*>(context))(type, component, data);
                             }});
        std::ranges::sort(found, {}, [](const auto& entry) { return entry.first->index; });
        for (const auto& [type, data] : found) {
            const auto [it, inserted] =
                type_index.try_emplace(type, static_cast<u32>(types.size()));
            if (inserted) {
                types.push_back(type);
            }
            components[i].push_back({.type = it->second, .data = data});
        }
    }

    serialize::BinaryWriter writer;
    writer.write(binary_magic);
    writer.write(static_cast<u32>(scene_version));
    writer.write(static_cast<u32>(types.size()));
    for (const reflect::TypeInfo* type : types) {
        writer.write_string(type->name);
        writer.write(layout_hash(*type));
    }
    writer.write(static_cast<u32>(entries.size()));
    for (usize i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        writer.write(entry.id);
        writer.write(entry.parent);
        writer.write(entry.parent_node);
        writer.write(entry.prefab);
        if (entry.prefab != 0) {
            // Überschreibungen sind selten und klein; als JSON-Text bleiben sie lesbar.
            writer.write_string(json::write(entry.overrides));
            continue;
        }
        writer.write(static_cast<u32>(components[i].size()));
        for (const Component& component : components[i]) {
            writer.write(component.type);
            // Länge vorweg, damit ein Leser die Komponente auch überspringen kann.
            serialize::BinaryWriter payload;
            serialize::write_binary(payload, component.data, *types[component.type]);
            writer.write(static_cast<u32>(payload.bytes().size()));
            writer.write_bytes(payload.bytes());
        }
    }
    return writer.take();
}

Result<usize> load_scene_binary(World& world, std::span<const std::byte> data,
                                const PrefabLookup& prefabs) {
    serialize::BinaryReader reader{data};
    u32 magic = 0;
    u32 version = 0;
    if (!reader.read(magic) || magic != binary_magic) {
        return std::unexpected(std::string{"Keine binäre NGine-Szene"});
    }
    if (!reader.read(version) || version != static_cast<u32>(scene_version)) {
        return std::unexpected(
            std::format("Szenenversion wird nicht unterstützt (erwartet {})", scene_version));
    }
    const auto broken = [] { return std::string{"Binäre Szene ist abgeschnitten oder kaputt"}; };
    u32 type_count = 0;
    if (!reader.read(type_count) || type_count > reader.remaining()) {
        return std::unexpected(broken());
    }
    std::vector<const reflect::TypeInfo*> types(type_count, nullptr);
    for (u32 i = 0; i < type_count; ++i) {
        std::string name;
        u64 hash = 0;
        if (!reader.read_string(name) || !reader.read(hash)) {
            return std::unexpected(broken());
        }
        const reflect::TypeInfo* type = world.types().find(name);
        if (type == nullptr || type->kind != reflect::Kind::Struct) {
            log::warn("Binäre Szene: Komponente '{}' ist unbekannt und wird übersprungen", name);
            continue;
        }
        if (layout_hash(*type) != hash) {
            return std::unexpected(
                std::format("Binäre Szene passt nicht zu diesem Build (Aufbau von '{}' geändert); "
                            "aus der JSON-Szene neu speichern",
                            name));
        }
        types[i] = type;
    }

    u32 count = 0;
    if (!reader.read(count) || count > reader.remaining()) {
        return std::unexpected(broken());
    }
    std::vector<LoadEntry> entries(count);
    std::vector<json::Value> overrides(count);
    for (u32 i = 0; i < count; ++i) {
        LoadEntry& entry = entries[i];
        i64 parent = 0;
        u32 parent_node = 0;
        if (!reader.read(entry.id) || !reader.read(parent) || !reader.read(parent_node) ||
            !reader.read(entry.prefab)) {
            return std::unexpected(broken());
        }
        if (parent != 0) {
            entry.parent = parent;
            entry.parent_node = parent_node;
        }
        if (entry.prefab != 0) {
            std::string text;
            if (!reader.read_string(text)) {
                return std::unexpected(broken());
            }
            auto parsed = json::parse(text);
            if (!parsed) {
                return std::unexpected(broken());
            }
            overrides[i] = *std::move(parsed);
            entry.overrides = &overrides[i];
            continue;
        }
        // Komponenten erst beim Anlegen lesen; hier nur ihren Bereich merken.
        const usize start = reader.position();
        u32 components = 0;
        if (!reader.read(components)) {
            return std::unexpected(broken());
        }
        for (u32 c = 0; c < components; ++c) {
            u32 type = 0;
            u32 size = 0;
            if (!reader.read(type) || !reader.read(size) || type >= type_count ||
                size > reader.remaining()) {
                return std::unexpected(broken());
            }
            std::vector<std::byte> skip(size);
            if (!reader.read_bytes(skip)) {
                return std::unexpected(broken());
            }
        }
        entry.binary = data.subspan(start, reader.position() - start);
    }
    if (reader.remaining() != 0) {
        return std::unexpected(broken());
    }

    return create_entries(
        world, entries, prefabs, [&](u64 entity, const LoadEntry& entry) -> Result<void> {
            serialize::BinaryReader in{entry.binary};
            u32 components = 0;
            static_cast<void>(in.read(components));
            for (u32 c = 0; c < components; ++c) {
                u32 index = 0;
                u32 size = 0;
                static_cast<void>(in.read(index));
                static_cast<void>(in.read(size));
                const std::span<const std::byte> bytes = entry.binary.subspan(in.position(), size);
                std::vector<std::byte> skip(size);
                static_cast<void>(in.read_bytes(skip));
                const reflect::TypeInfo* type = types[index];
                if (type == nullptr) {
                    continue;
                }
                const ComponentId component = world.component(*type);
                void* target = world.ensure(entity, component);
                serialize::BinaryReader payload{bytes};
                if (auto result = serialize::read_binary(payload, target, *type); !result) {
                    return std::unexpected(std::format("{}: {}", type->name, result.error()));
                }
                world.modified(entity, component);
            }
            return {};
        });
}

bool is_binary_scene_path(std::string_view path) {
    return filesystem::extension(path) == binary_scene_extension;
}

Result<void> save_scene_file(const World& world, std::string_view path,
                             const PrefabLookup& prefabs) {
    if (is_binary_scene_path(path)) {
        return filesystem::write_file(path, save_scene_binary(world, prefabs));
    }
    return filesystem::write_text(path, save_scene(world, prefabs));
}

Result<usize> load_scene_file(World& world, std::string_view path, const PrefabLookup& prefabs) {
    auto bytes = filesystem::read_file(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    // Am Inhalt erkennen, nicht an der Endung: „NGSC“ am Anfang heißt binär.
    if (bytes->size() >= 4 && (*bytes)[0] == std::byte{'N'} && (*bytes)[1] == std::byte{'G'} &&
        (*bytes)[2] == std::byte{'S'} && (*bytes)[3] == std::byte{'C'}) {
        return load_scene_binary(world, *bytes, prefabs);
    }
    return load_scene(world,
                      std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()},
                      prefabs);
}

} // namespace ngine::world
