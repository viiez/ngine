module;

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

export module ngine.edit:commands;

import ngine.core;
import ngine.reflect;
import ngine.world;
import :document;

export namespace ngine::edit {

// Weg von einer Komponente zu einem ihrer Felder, auch verschachtelt: Indizes in
// TypeInfo::fields, von der Komponente abwärts. Ohne Indizes meint er die ganze Komponente.
struct FieldPath {
    reflect::TypeId component = 0;
    std::vector<u32> fields;

    friend bool operator==(const FieldPath&, const FieldPath&) = default;
};

// Was ein FieldPath in der Reflection bedeutet.
struct ResolvedField {
    const reflect::TypeInfo* component = nullptr;
    const reflect::TypeInfo* type = nullptr;
    // Feld am Ende des Weges, nullptr für die ganze Komponente.
    const reflect::Field* field = nullptr;
    // Abstand vom Anfang der Komponente.
    usize offset = 0;
};

[[nodiscard]] Result<ResolvedField> resolve(const reflect::TypeRegistry& types,
                                            const FieldPath& path);
// Lesbar, z. B. „Transform.position“.
[[nodiscard]] std::string to_string(const reflect::TypeRegistry& types, const FieldPath& path);

// Wert eines reflektierten Typs als Bytes im Binärformat und zurück.
[[nodiscard]] std::vector<std::byte> capture(const void* value, const reflect::TypeInfo& type);
[[nodiscard]] Result<void> restore(void* target, const reflect::TypeInfo& type,
                                   std::span<const std::byte> bytes);

[[nodiscard]] EntitySnapshot snapshot(SceneDocument& document, EntityId root);
// Legt den Teilbaum mit denselben Kennungen neu an. Das Elternteil muss existieren (oder 0 sein).
[[nodiscard]] Result<void> restore(SceneDocument& document, const EntitySnapshot& snapshot);

// --- Befehle ---------------------------------------------------------------------------------
// Fabriken für die Befehle des Editors. Ausgeführt werden sie über SceneDocument::execute().

// Setzt ein Feld (oder mit leerem Weg die ganze Komponente) auf value; value zeigt auf einen Wert
// des Feldtyps. Liefert nullptr, wenn der Wert schon so ist. Der alte Wert wird jetzt gelesen.
// label ersetzt die Beschriftung „<Feld> ändern“ (z. B. „Verschieben“ beim Gizmo).
[[nodiscard]] Result<std::unique_ptr<Command>> set_field(SceneDocument& document, EntityId entity,
                                                         const FieldPath& path, const void* value,
                                                         std::string label = {});

// Setzt die Komponente Name (legt sie bei Bedarf an).
[[nodiscard]] std::unique_ptr<Command> rename(EntityId entity, std::string name);

// Hängt entity unter parent (0: Wurzel). Die Lage in der Welt bleibt (TD-28): Transform wird so
// umgerechnet, dass die Entity nicht springt. Scherung durch ungleichmäßig skalierte, gedrehte
// Eltern geht dabei verloren. Mit keep_world false bleibt die lokale Lage.
[[nodiscard]] std::unique_ptr<Command> reparent(EntityId entity, EntityId parent,
                                                bool keep_world = true);

// Neue Entity mit Name und Transform unter parent (0: Wurzel). id ist ihre Kennung, schon bevor
// der Befehl läuft.
struct CreatedEntity {
    std::unique_ptr<Command> command;
    EntityId id = 0;
};
[[nodiscard]] CreatedEntity create_entity(SceneDocument& document, std::string name,
                                          EntityId parent = 0);

// Löscht entity mit allen Kindern.
[[nodiscard]] std::unique_ptr<Command> destroy_entity(EntityId entity);

// Legt einen Teilbaum an, z. B. ein Modell aus dem Asset-Browser (R2.4). Die Kennungen im Snapshot
// müssen frei sein (SceneDocument::reserve_id()), der erste Knoten ist die Wurzel; Eltern stehen
// vor ihren Kindern. Undo löscht den Teilbaum wieder.
[[nodiscard]] std::unique_ptr<Command> insert_entities(EntitySnapshot snapshot, std::string label);

// Fügt eine Komponente mit Standardwert hinzu bzw. entfernt sie.
[[nodiscard]] std::unique_ptr<Command> add_component(EntityId entity, reflect::TypeId component);
[[nodiscard]] std::unique_ptr<Command> remove_component(EntityId entity, reflect::TypeId component);

// Mehrere Befehle als ein Schritt, ausgeführt in der Reihenfolge, zurückgenommen umgekehrt.
[[nodiscard]] std::unique_ptr<Command> group(std::string label,
                                             std::vector<std::unique_ptr<Command>> commands);

} // namespace ngine::edit
