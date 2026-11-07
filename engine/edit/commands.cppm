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

// Teilbaum einer Entity mit allen reflektierten Komponenten (auch Laufzeitkomponenten wie
// MeshRenderer), Eltern vor ihren Kindern. Damit macht Undo das Löschen rückgängig.
struct EntitySnapshot {
    struct Component {
        reflect::TypeId type = 0;
        std::vector<std::byte> bytes;
    };
    struct Node {
        EntityId id = 0;
        // Elternteil im Dokument; beim ersten Knoten das Elternteil des ganzen Teilbaums.
        EntityId parent = 0;
        std::vector<Component> components;
    };
    std::vector<Node> nodes;
};

[[nodiscard]] EntitySnapshot snapshot(SceneDocument& document, EntityId root);
// Legt den Teilbaum mit denselben Kennungen neu an. Das Elternteil muss existieren (oder 0 sein).
[[nodiscard]] Result<void> restore(SceneDocument& document, const EntitySnapshot& snapshot);

// --- Befehle ---------------------------------------------------------------------------------
// Fabriken für die Befehle des Editors. Ausgeführt werden sie über SceneDocument::execute().

// Setzt ein Feld (oder mit leerem Weg die ganze Komponente) auf value; value zeigt auf einen Wert
// des Feldtyps. Liefert nullptr, wenn der Wert schon so ist. Der alte Wert wird jetzt gelesen.
[[nodiscard]] Result<std::unique_ptr<Command>> set_field(SceneDocument& document, EntityId entity,
                                                         const FieldPath& path, const void* value);

// Setzt die Komponente Name (legt sie bei Bedarf an).
[[nodiscard]] std::unique_ptr<Command> rename(EntityId entity, std::string name);

// Hängt entity unter parent (0: Wurzel). Die lokale Lage (Transform) bleibt, wie sie ist.
[[nodiscard]] std::unique_ptr<Command> reparent(EntityId entity, EntityId parent);

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

// Fügt eine Komponente mit Standardwert hinzu bzw. entfernt sie.
[[nodiscard]] std::unique_ptr<Command> add_component(EntityId entity, reflect::TypeId component);
[[nodiscard]] std::unique_ptr<Command> remove_component(EntityId entity, reflect::TypeId component);

// Mehrere Befehle als ein Schritt, ausgeführt in der Reihenfolge, zurückgenommen umgekehrt.
[[nodiscard]] std::unique_ptr<Command> group(std::string label,
                                             std::vector<std::unique_ptr<Command>> commands);

} // namespace ngine::edit
