module;

#include <cstdint>
#include <vector>

// Funktionen für Skript-Komponenten (R4.4): Skript-Klassen als zusammengesetzte Typen in der
// Registry anmelden, Standardwerte und Texte schreiben. Interne Partition; die Tabelle ScriptApi
// steht in NativeApi und hat dieselbe Anordnung wie NGine.ScriptApi in managed/ScriptClasses.cs.
module ngine.script:script_api;

import ngine.core;
import ngine.reflect;

namespace ngine::script {

struct ScriptFieldDesc {
    const u8* name = nullptr;
    std::int32_t name_length = 0;
    const u8* type = nullptr;
    std::int32_t type_length = 0;
    std::int32_t hint = 0;
    std::int32_t has_range = 0;
    f64 min = 0.0;
    f64 max = 0.0;
    f64 step = 0.0;
};

struct ScriptApi {
    // Index des Typs in der Registry oder -1.
    std::int32_t (*register_script)(const u8* name, std::int32_t length,
                                    const ScriptFieldDesc* fields, std::int32_t count);
    // Offset eines Feldes; field = -1 liefert die Größe des Typs.
    std::int64_t (*field_offset)(std::int32_t type, std::int32_t field);
    void* (*prototype)(std::int32_t type);
    std::int32_t (*string_get)(const void* string, const u8** out);
    void (*string_set)(void* string, const u8* text, std::int32_t length);
};

[[nodiscard]] ScriptApi make_script_api();

// Registry, in der Skript-Typen entstehen; nullptr: keine Skript-Komponenten.
void set_script_registry(reflect::TypeRegistry* types);
[[nodiscard]] reflect::TypeRegistry* script_registry() noexcept;
// Zieht Entities der gebundenen Welt von ersetzten Skript-Typen auf ihre Nachfolger um (nach
// RegisterScripts, wenn C# die Standardwerte geschrieben hat). Liefert die Zahl der Komponenten.
usize migrate_script_types();
// Skript-Typen in der Reihenfolge ihrer Anmeldung (aktuelle, nicht ersetzte).
[[nodiscard]] const std::vector<const reflect::TypeInfo*>& registered_script_types() noexcept;
void clear_registered_script_types();

} // namespace ngine::script
