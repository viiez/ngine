module;

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
    i32 name_length = 0;
    const u8* type = nullptr;
    i32 type_length = 0;
    i32 hint = 0;
    i32 has_range = 0;
    f64 min = 0.0;
    f64 max = 0.0;
    f64 step = 0.0;
};

struct ScriptApi {
    // Index des Typs in der Registry oder -1.
    i32 (*register_script)(const u8* name, i32 length, const ScriptFieldDesc* fields, i32 count);
    // Offset eines Feldes; field = -1 liefert die Größe des Typs.
    i64 (*field_offset)(i32 type, i32 field);
    void* (*prototype)(i32 type);
    i32 (*string_get)(const void* string, const u8** out);
    void (*string_set)(void* string, const u8* text, i32 length);
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
