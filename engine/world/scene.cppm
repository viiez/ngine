module;

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module ngine.world:scene;

import ngine.core;
import :ecs;
import :prefab;

// Szenen speichern und laden (WORLD-05). Eine Szene ist JSON (ADR 0020):
//
//   {
//     "format": "ngine.scene",
//     "version": 1,
//     "entities": [
//       { "id": 1, "components": { "Name": { "value": "Sonne" } } },
//       { "id": 2, "parent": 1, "components": { ... } }
//     ]
//   }
//
// Die IDs gelten nur in der Datei. Eltern stehen vor ihren Kindern, Kinder in der Reihenfolge ihrer
// Kennungen, Komponenten in der Reihenfolge der Registrierung; dieselbe Welt ergibt deshalb immer
// denselben Text, und eine geladene Szene wird ohne Änderung textgleich gespeichert.
//
// Seit R2.5 (WORLD-04) steht eine Prefab-Instanz als ein Eintrag mit der Kennung der Vorlage und
// ihren Überschreibungen in der Datei, ohne ihre Knoten:
//
//   { "id": 3, "parent": 1, "prefab": 1234, "overrides": [ { "node": 1, "components": {...} } ] }
//
// Entities, die jemand unter einen Knoten einer Instanz gehängt hat, verweisen mit "parent" auf
// die Instanz und mit "parent_node" auf den Knoten. Dazu gibt es ein binäres Format (.nscene) mit
// demselben Inhalt, Komponenten ohne Feldnamen im Binärformat der Reflection (ADR 0020): schneller
// zu laden, aber nur für einen Build mit denselben Komponenten.
export namespace ngine::world {

inline constexpr std::string_view scene_format = "ngine.scene";
inline constexpr i64 scene_version = 1;
inline constexpr std::string_view binary_scene_extension = ".nscene";

// Schreibt alle Entities mit ihren reflektierten Komponenten und Eltern. prefabs liefert die
// Vorlagen der Instanzen, gegen die die Überschreibungen gerechnet werden.
[[nodiscard]] std::string save_scene(const World& world, const PrefabLookup& prefabs = {});

// Legt die Entities der Szene zusätzlich zu den vorhandenen an und liefert, wie viele Einträge es
// waren. Unbekannte Komponenten werden mit Warnung übersprungen; fehlt eine Vorlage, bleibt von der
// Instanz nur die Wurzel mit ihren Überschreibungen als Text. Bei einem Fehler bleibt das bisher
// Geladene in der Welt.
[[nodiscard]] Result<usize> load_scene(World& world, std::string_view text,
                                       const PrefabLookup& prefabs = {});

// Dasselbe im Binärformat.
[[nodiscard]] std::vector<std::byte> save_scene_binary(const World& world,
                                                       const PrefabLookup& prefabs = {});
[[nodiscard]] Result<usize> load_scene_binary(World& world, std::span<const std::byte> data,
                                              const PrefabLookup& prefabs = {});

// Endet der Pfad auf .nscene?
[[nodiscard]] bool is_binary_scene_path(std::string_view path);

// Speichert binär, wenn der Pfad auf .nscene endet, sonst als JSON. Laden erkennt das Format am
// Inhalt.
[[nodiscard]] Result<void> save_scene_file(const World& world, std::string_view path,
                                           const PrefabLookup& prefabs = {});
[[nodiscard]] Result<usize> load_scene_file(World& world, std::string_view path,
                                            const PrefabLookup& prefabs = {});

} // namespace ngine::world
