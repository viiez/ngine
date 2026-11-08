module;

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module ngine.world:prefab;

import ngine.core;
import ngine.reflect;
import ngine.serialize;
import :ecs;

// Prefabs (WORLD-04, ADR 0037): Vorlagen für Teilbäume aus Entities als Datei, Instanzen davon
// in Szenen mit Überschreibungen. Eine Prefab-Datei hat das Format einer Szene mit genau einer
// Wurzel:
//
//   { "format": "ngine.prefab", "version": 1,
//     "entities": [ { "id": 1, "components": { ... } }, { "id": 2, "parent": 1, ... } ] }
//
// Die IDs der Datei sind die Knoten der Vorlage. In der Welt ist eine Instanz voll ausgebaut: jede
// Entity trägt PrefabNode mit ihrem Knoten, die Wurzel zusätzlich PrefabInstance mit der Kennung
// der Vorlage (Asset). Szenen speichern von einer Instanz nur die Kennung und die Felder, die von
// der Vorlage abweichen (Überschreibungen); ändert sich die Vorlage, ziehen alle Instanzen nach und
// behalten ihre Überschreibungen.
export namespace ngine::world {

template <typename T>
using Result = std::expected<T, std::string>;

inline constexpr std::string_view prefab_format = "ngine.prefab";
inline constexpr i64 prefab_version = 1;

// Wurzel einer Prefab-Instanz. Nur zur Laufzeit: Szenen speichern sie als "prefab" an der Entity.
struct PrefabInstance {
    u64 prefab = 0;
};

// Knoten der Vorlage, den eine Entity einer Instanz darstellt (ID in der Prefab-Datei).
struct PrefabNode {
    u32 node = 0;
};

// Überschreibungen einer Instanz, deren Vorlage beim Laden fehlte, als JSON-Text. Szenen speichern
// sie unverändert zurück, damit nichts verloren geht.
struct PrefabOverrides {
    std::string json;
};

// Eine Vorlage im Speicher: Knoten in der Reihenfolge der Datei, Eltern vor Kindern, der erste
// ist die Wurzel. components ist ein JSON-Objekt wie in Szenen.
struct Prefab {
    struct Node {
        u32 id = 0;
        // Index des Elternknotens in nodes; nichts bei der Wurzel.
        std::optional<usize> parent;
        json::Value components{json::Object{}};
    };
    std::vector<Node> nodes;

    [[nodiscard]] const Node* find(u32 id) const noexcept {
        for (const Node& node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    }
};

[[nodiscard]] Result<Prefab> parse_prefab(std::string_view text);
[[nodiscard]] Result<Prefab> load_prefab_file(std::string_view path);
[[nodiscard]] std::string write_prefab(const Prefab& prefab);

// Vorlage aus dem Teilbaum unter root. Die Wurzel kommt in den Ursprung (Position 0; Drehung und
// Größe bleiben), Instanzen darin werden aufgelöst (verschachtelte Prefabs gibt es noch nicht).
[[nodiscard]] Prefab make_prefab(const World& world, u64 root);

// Vorlage zu einer Kennung, nullptr wenn es sie nicht gibt.
using PrefabLookup = std::function<const Prefab*(u64 prefab)>;

// Reflektierte, nicht transiente Komponenten einer Entity als JSON-Objekt (Name → Werte), in der
// Reihenfolge der Registrierung.
[[nodiscard]] json::Value components_to_json(const World& world, u64 entity);
// Setzt Komponenten aus einem JSON-Objekt; Teilobjekte ändern nur die genannten Felder.
// Unbekannte Komponenten werden mit Warnung übersprungen. context steht in Meldungen.
[[nodiscard]] Result<void> components_from_json(World& world, u64 entity,
                                                const json::Value& components,
                                                std::string_view context);

// Wurzel der Instanz, zu der entity gehört (entity selbst oder ein Vorfahr mit PrefabInstance),
// 0 wenn sie zu keiner gehört.
[[nodiscard]] u64 instance_root(const World& world, u64 entity);
// Entities einer Instanz mit ihren Knoten, Eltern vor Kindern.
[[nodiscard]] std::vector<std::pair<u32, u64>> instance_nodes(const World& world, u64 root);

// Überschreibungen der Instanz unter root gegenüber prefab als JSON-Array
// [{ "node": 2, "components": { "PointLight": { "color": ... } } }], nur Felder, die abweichen,
// und Komponenten, die die Vorlage nicht hat. Leeres Array ohne Abweichungen.
[[nodiscard]] json::Value prefab_overrides(const World& world, u64 root, const Prefab& prefab);

// Legt eine Instanz unter parent (0: Wurzel) an und wendet overrides an (Format wie
// prefab_overrides(); nullptr: keine).
[[nodiscard]] Result<Entity> instantiate(World& world, const Prefab& prefab, u64 prefab_id,
                                         const json::Value* overrides = nullptr, u64 parent = 0);

// Bringt alle Instanzen von prefab_id von before auf after: Werte der Vorlage ändern sich,
// Überschreibungen bleiben, neue Knoten kommen dazu, weggefallene verschwinden. Entities, die es
// weiter gibt, behalten ihre Kennung. Liefert die Zahl der Instanzen.
usize update_instances(World& world, u64 prefab_id, const Prefab& before, const Prefab& after);

// Bricht eine Instanz auf: Die Entities bleiben, wie sie sind, die Verknüpfung mit der Vorlage
// (PrefabInstance, PrefabNode) verschwindet.
void unpack_instance(World& world, u64 root);

// Registriert PrefabInstance, PrefabNode und PrefabOverrides (aus world::register_types()).
void register_prefab_types(reflect::TypeRegistry& types);

} // namespace ngine::world

namespace ngine::world {

// Wie instantiate(), aber mit einer schon angelegten, leeren Entity als Wurzel (0: neu anlegen).
// Szenen laden so Instanzen an der Stelle, die ihr Eintrag in der Datei hat.
[[nodiscard]] Result<Entity> instantiate_at(World& world, const Prefab& prefab, u64 prefab_id,
                                            const json::Value* overrides, u64 parent, u64 root);

} // namespace ngine::world
