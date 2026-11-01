module;

#include <cstddef>
#include <memory>
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

// Grundarten von Typen. Ganz- und Gleitkommazahlen unterscheiden sich nur in der Größe.
enum class Kind : u8 {
    Bool,
    Int,
    UInt,
    Float,
    String,
    Struct,
};

struct TypeInfo;

struct Field {
    std::string name;
    const TypeInfo* type = nullptr;
    // Abstand vom Anfang des Objekts in Bytes.
    usize offset = 0;
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
    TypeOps ops{};
    // Nur bei Kind::Struct, in der Reihenfolge der Registrierung.
    std::vector<Field> fields;

    [[nodiscard]] const Field* find_field(std::string_view field_name) const noexcept {
        for (const Field& field : fields) {
            if (field.name == field_name) {
                return &field;
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
    } else {
        return Kind::Struct;
    }
}

// Offset eines Feldes aus einem Zeiger auf ein Datenelement. offsetof() nimmt keine Zeiger auf
// Elemente; deshalb wird ein Objekt erzeugt und die Adressen verglichen.
template <typename T, typename M>
[[nodiscard]] usize offset_of(M T::* member) {
    const T object{};
    const auto* base = reinterpret_cast<const std::byte*>(std::addressof(object));
    const auto* field = reinterpret_cast<const std::byte*>(std::addressof(object.*member));
    return static_cast<usize>(field - base);
}

} // namespace ngine::reflect
