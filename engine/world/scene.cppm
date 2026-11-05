module;

#include <expected>
#include <string>
#include <string_view>

export module ngine.world:scene;

import ngine.core;
import :ecs;

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
export namespace ngine::world {

template <typename T>
using Result = std::expected<T, std::string>;

inline constexpr std::string_view scene_format = "ngine.scene";
inline constexpr i64 scene_version = 1;

// Schreibt alle Entities mit ihren reflektierten Komponenten und Eltern.
[[nodiscard]] std::string save_scene(const World& world);

// Legt die Entities der Szene zusätzlich zu den vorhandenen an und liefert, wie viele es waren.
// Unbekannte Komponenten werden mit Warnung übersprungen. Bei einem Fehler bleibt das bisher
// Geladene in der Welt.
[[nodiscard]] Result<usize> load_scene(World& world, std::string_view text);

[[nodiscard]] Result<void> save_scene_file(const World& world, std::string_view path);
[[nodiscard]] Result<usize> load_scene_file(World& world, std::string_view path);

} // namespace ngine::world
