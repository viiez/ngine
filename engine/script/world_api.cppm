module;

#include <cstdint>

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
    std::int32_t (*type_index)(const u8* name, std::int32_t length);
    // Zeiger auf die Komponente oder nullptr. write markiert sie als geändert.
    void* (*get)(u64 entity, std::int32_t type, std::int32_t write);
    std::int32_t (*has)(u64 entity, std::int32_t type);
    // Setzt die Komponente: vorhanden sofort, sonst über den Befehlspuffer. 0: gut.
    std::int32_t (*set)(u64 entity, std::int32_t type, const void* value);
    std::int32_t (*remove)(u64 entity, std::int32_t type);
    u64 (*create)(const u8* name, std::int32_t length);
    std::int32_t (*destroy)(u64 entity);
    std::int32_t (*alive)(u64 entity);
    u64 (*parent)(u64 entity);
    std::int32_t (*set_parent)(u64 child, u64 parent);
    std::int32_t (*children)(u64 entity, u64* out, std::int32_t capacity);
    // Länge des Namens, Text in *out (gilt bis zur nächsten Strukturänderung); -1 ohne Namen.
    std::int32_t (*name)(u64 entity, const u8** out);
    std::int32_t (*set_name)(u64 entity, const u8* name, std::int32_t length);
    u64 (*find)(const u8* name, std::int32_t length);
    // Anzahl der Chunks; *out gilt bis zur nächsten Abfrage mit denselben Typen oder
    // Strukturänderung.
    std::int32_t (*query)(const std::int32_t* types, std::int32_t count, const NativeChunk** out);
};

[[nodiscard]] WorldApi make_world_api();

// Welt, auf die die Funktionen wirken; nullptr löst sie.
void bind_script_world(world::World* world);
[[nodiscard]] world::World* script_world() noexcept;
// Führt die gesammelten Strukturänderungen aus.
void apply_script_commands();

} // namespace ngine::script
