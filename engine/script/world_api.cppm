// Funktionen der Welt für C# (R4.4): Entities, Komponenten, Abfragen. Interne Partition; die
// Tabelle WorldApi steht in NativeApi (runtime.cpp) und hat dieselbe Anordnung wie
// NGine.WorldApi in managed/Native.cs.
module ngine.script:world_api;

import ngine.core;
import ngine.world;

namespace ngine::script {

// Ein Block von Entities mit denselben Komponenten, wie C# ihn sieht (NGine.NativeChunk).
struct NativeChunk {
    u32 count = 0;
    u32 reserved = 0;
    const u64* entities = nullptr;
    void* columns[world::max_query_terms] = {};
};

// Komponenten werden über den Index ihres Typs in der Registry angesprochen (TypeInfo::index);
// -1 heißt unbekannt.
struct WorldApi {
    i32 (*type_index)(const u8* name, i32 length);
    // Zeiger auf die Komponente oder nullptr. write markiert sie als geändert.
    void* (*get)(u64 entity, i32 type, i32 write);
    i32 (*has)(u64 entity, i32 type);
    // Setzt die Komponente: vorhanden sofort, sonst über den Befehlspuffer. 0: gut.
    i32 (*set)(u64 entity, i32 type, const void* value);
    i32 (*remove)(u64 entity, i32 type);
    u64 (*create)(const u8* name, i32 length);
    i32 (*destroy)(u64 entity);
    i32 (*alive)(u64 entity);
    u64 (*parent)(u64 entity);
    i32 (*set_parent)(u64 child, u64 parent);
    i32 (*children)(u64 entity, u64* out, i32 capacity);
    // Länge des Namens, Text in *out (gilt bis zur nächsten Strukturänderung); -1 ohne Namen.
    i32 (*name)(u64 entity, const u8** out);
    i32 (*set_name)(u64 entity, const u8* name, i32 length);
    u64 (*find)(const u8* name, i32 length);
    // Anzahl der Chunks; *out gilt bis zur nächsten Abfrage mit denselben Typen oder
    // Strukturänderung.
    i32 (*query)(const i32* types, i32 count, const NativeChunk** out);
};

[[nodiscard]] WorldApi make_world_api();

// Welt, auf die die Funktionen wirken; nullptr löst sie.
void bind_script_world(world::World* world);
[[nodiscard]] world::World* script_world() noexcept;
// Führt die gesammelten Strukturänderungen aus.
void apply_script_commands();

// Skript-Welten (R5.5): Jede Partition der API hält ihren Zustand pro Skript-Welt; diese Kennung
// wählt, welcher gilt. 0 ist die Welt ohne ScriptSystem (D2, Tests der Laufzeit).
[[nodiscard]] u32 script_domain() noexcept;
void set_script_domain(u32 domain) noexcept;
// Vergisst den Zustand einer Skript-Welt in dieser Partition.
void drop_world_domain(u32 domain);
// Skripte eines Clients: Schreiben in replizierte Komponenten warnt (einmal pro Typ), außer
// während store_fields() (set_script_storing).
void set_script_client(bool client) noexcept;
void set_script_storing(bool storing) noexcept;
// Typen, für die schon gewarnt wurde (Tests).
[[nodiscard]] usize script_write_warnings() noexcept;

} // namespace ngine::script
