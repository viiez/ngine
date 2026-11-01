module;

#include <expected>
#include <string>

export module ngine.serialize:reflected;

import ngine.core;
import ngine.reflect;
import :json;
import :binary;

// Serialisierung beliebiger reflektierter Typen. Die Funktionen arbeiten auf rohem Speicher mit
// einem TypeInfo; die Templates darüber sind nur Bequemlichkeit.
export namespace ngine::serialize {

template <typename T>
using Result = std::expected<T, std::string>;

// Structs werden zu Objekten mit einem Schlüssel pro Feld, Grundtypen zu JSON-Werten.
[[nodiscard]] json::Value to_json(const void* object, const reflect::TypeInfo& type);

// Füllt ein vorhandenes Objekt. Fehlende Felder behalten ihren Wert, unbekannte Schlüssel werden
// übersprungen; so lassen sich Dateien weiter lesen, wenn Felder dazukommen oder wegfallen. Passt
// ein Wert nicht (falscher Typ, Zahl außerhalb des Bereichs), nennt der Fehler den Pfad zum Feld.
[[nodiscard]] Result<void> from_json(const json::Value& value, void* object,
                                     const reflect::TypeInfo& type);

// Alle Felder in der Reihenfolge der Registrierung, ohne Namen.
void write_binary(BinaryWriter& writer, const void* object, const reflect::TypeInfo& type);
[[nodiscard]] Result<void> read_binary(BinaryReader& reader, void* object,
                                       const reflect::TypeInfo& type);

template <typename T>
[[nodiscard]] json::Value to_json(const T& object, const reflect::TypeRegistry& types) {
    return to_json(&object, types.get<T>());
}

template <typename T>
[[nodiscard]] Result<void> from_json(const json::Value& value, T& object,
                                     const reflect::TypeRegistry& types) {
    return from_json(value, &object, types.get<T>());
}

template <typename T>
void write_binary(BinaryWriter& writer, const T& object, const reflect::TypeRegistry& types) {
    write_binary(writer, &object, types.get<T>());
}

template <typename T>
[[nodiscard]] Result<void> read_binary(BinaryReader& reader, T& object,
                                       const reflect::TypeRegistry& types) {
    return read_binary(reader, &object, types.get<T>());
}

} // namespace ngine::serialize
