module;

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module ngine.reflect:type;

import ngine.core;

export namespace ngine::reflect {

// Stabile Kennung eines Typs: FNV-1a-Hash seines Namens. Bleibt über Programmstarts, Builds und
// Plattformen gleich, anders als Adressen oder std::type_info.
using TypeId = u64;

[[nodiscard]] constexpr TypeId type_id(std::string_view name) noexcept {
    u64 hash = 14695981039346656037ull;
    for (const char c : name) {
        hash ^= static_cast<u8>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

// Grundarten von Typen. Ganz- und Gleitkommazahlen unterscheiden sich nur in der Größe; Enums
// speichern ihren Wert als Ganzzahl der Größe TypeInfo::size.
enum class Kind : u8 {
    Bool,
    Int,
    UInt,
    Float,
    String,
    Enum,
    Struct,
};

struct TypeInfo;

// Gültiger Bereich eines Zahlenfeldes, beide Grenzen eingeschlossen. Der Editor klemmt Eingaben
// darauf; Laden und Netzwerk lassen Werte, wie sie sind. step ist die Schrittweite beim Ziehen im
// Inspector (0: automatisch).
struct Range {
    f64 min = 0.0;
    f64 max = 0.0;
    f64 step = 0.0;
};

// Wie der Editor ein Feld darstellt, wenn die Art allein nicht reicht.
enum class Hint : u8 {
    None,
    // Vektor aus drei oder vier Gleitkommazahlen als lineare Farbe (Farbwähler).
    Color,
    // Quaternion, im Editor als Eulerwinkel in Grad.
    Rotation,
    // Gleitkommazahl im Bogenmaß, im Editor in Grad.
    Angle,
};

struct Field {
    std::string name;
    const TypeInfo* type = nullptr;
    // Abstand vom Anfang des Objekts in Bytes.
    usize offset = 0;
    // Nur bei Zahlen sinnvoll; fehlt er, ist jeder Wert des Typs erlaubt.
    std::optional<Range> range;
    Hint hint = Hint::None;
};

// Ein benannter Wert eines Enums.
struct Enumerator {
    std::string name;
    i64 value = 0;
};

// Lebenszyklus eines Typs, ohne ihn zu kennen. Alle Zeiger zeigen auf passend ausgerichteten
// Speicher der Größe TypeInfo::size.
struct TypeOps {
    void (*construct)(void* target);
    void (*destroy)(void* target) noexcept;
    void (*copy_construct)(void* target, const void* source);
    void (*move_construct)(void* target, void* source) noexcept;
    void (*copy_assign)(void* target, const void* source);
    void (*move_assign)(void* target, void* source) noexcept;
};

struct TypeInfo {
    std::string name;
    TypeId id = 0;
    usize size = 0;
    usize alignment = 0;
    Kind kind = Kind::Struct;
    // Lässt sich mit memcpy kopieren und braucht keinen Destruktor.
    bool trivial = false;
    // Nur zur Laufzeit: Szenen speichern solche Komponenten nicht, weil sie aus anderen berechnet
    // werden (z. B. WorldTransform aus Transform und Hierarchie).
    bool transient = false;
    TypeOps ops{};
    // Nur bei Kind::Struct, in der Reihenfolge der Registrierung.
    std::vector<Field> fields;
    // Nur bei Kind::Enum, in der Reihenfolge der Registrierung.
    std::vector<Enumerator> enumerators;
    // Platz in der Reihenfolge der Registrierung (TypeRegistry::types()). Fest, solange die
    // Registry lebt; taugt als Index in Arrays pro Typ.
    u32 index = 0;

    [[nodiscard]] const Field* find_field(std::string_view field_name) const noexcept {
        for (const Field& field : fields) {
            if (field.name == field_name) {
                return &field;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Enumerator*
    find_enumerator(std::string_view enumerator_name) const noexcept {
        for (const Enumerator& enumerator : enumerators) {
            if (enumerator.name == enumerator_name) {
                return &enumerator;
            }
        }
        return nullptr;
    }
    [[nodiscard]] const Enumerator* find_enumerator(i64 value) const noexcept {
        for (const Enumerator& enumerator : enumerators) {
            if (enumerator.value == value) {
                return &enumerator;
            }
        }
        return nullptr;
    }
};

// Was ein Typ können muss, damit er reflektiert werden kann: Komponenten werden in Tabellen
// erzeugt, kopiert und verschoben, ohne dass eine Ausnahme dazwischenkommen darf.
template <typename T>
concept Reflectable = std::is_default_constructible_v<T> && std::is_copy_constructible_v<T> &&
                      std::is_copy_assignable_v<T> && std::is_nothrow_move_constructible_v<T> &&
                      std::is_nothrow_move_assignable_v<T> && std::is_nothrow_destructible_v<T> &&
                      !std::is_const_v<T> && !std::is_reference_v<T>;

template <Reflectable T>
[[nodiscard]] constexpr TypeOps make_ops() noexcept {
    return TypeOps{
        .construct = [](void* target) { std::construct_at(static_cast<T*>(target)); },
        .destroy = [](void* target) noexcept { std::destroy_at(static_cast<T*>(target)); },
        .copy_construct =
            [](void* target, const void* source) {
                std::construct_at(static_cast<T*>(target), *static_cast<const T*>(source));
            },
        .move_construct =
            [](void* target, void* source) noexcept {
                std::construct_at(static_cast<T*>(target), std::move(*static_cast<T*>(source)));
            },
        .copy_assign =
            [](void* target, const void* source) {
                *static_cast<T*>(target) = *static_cast<const T*>(source);
            },
        .move_assign =
            [](void* target, void* source) noexcept {
                *static_cast<T*>(target) = std::move(*static_cast<T*>(source));
            },
    };
}

template <Reflectable T>
[[nodiscard]] constexpr Kind kind_of() noexcept {
    if constexpr (std::is_same_v<T, bool>) {
        return Kind::Bool;
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        return Kind::Int;
    } else if constexpr (std::is_integral_v<T>) {
        return Kind::UInt;
    } else if constexpr (std::is_floating_point_v<T>) {
        return Kind::Float;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return Kind::String;
    } else if constexpr (std::is_enum_v<T>) {
        return Kind::Enum;
    } else {
        return Kind::Struct;
    }
}

// Offset eines Feldes aus einem Zeiger auf ein Datenelement. offsetof() nimmt keine Zeiger auf
// Elemente; deshalb wird ein Objekt erzeugt und die Adressen verglichen.
template <typename T, typename M>
[[nodiscard]] usize offset_of(M T::*member) {
    const T object{};
    const auto* base = reinterpret_cast<const std::byte*>(std::addressof(object));
    const auto* field = reinterpret_cast<const std::byte*>(std::addressof(object.*member));
    return static_cast<usize>(field - base);
}

} // namespace ngine::reflect
