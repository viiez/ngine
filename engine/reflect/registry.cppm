module;

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

export module ngine.reflect:registry;

import ngine.core;
import :type;

namespace ngine::reflect::detail {

// Eindeutige Adresse pro C++-Typ, ohne RTTI.
template <typename T>
inline constexpr char type_key_tag = 0;

} // namespace ngine::reflect::detail

export namespace ngine::reflect {

// Schlüssel eines C++-Typs innerhalb eines Programmlaufs. Nicht stabil, nicht speichern; dafür
// gibt es TypeId.
using TypeKey = const void*;

template <typename T>
[[nodiscard]] constexpr TypeKey type_key() noexcept {
    return &detail::type_key_tag<std::remove_cv_t<T>>;
}

class TypeRegistry;

// Fügt einem registrierten Struct Felder hinzu:
//   types.add<Velocity>("Velocity").field("x", &Velocity::x).field("y", &Velocity::y);
template <typename T>
class TypeBuilder {
public:
    // member ist ein Zeiger auf ein Datenelement (&T::feld). Der Typ des Feldes muss schon
    // registriert sein (Grundtypen sind es immer).
    template <typename Member>
        requires std::is_member_object_pointer_v<Member>
    TypeBuilder& field(std::string_view name, Member member);

    // Markiert den Typ als nur zur Laufzeit vorhanden (TypeInfo::transient).
    TypeBuilder& transient() noexcept {
        info_->transient = true;
        return *this;
    }

    [[nodiscard]] const TypeInfo& info() const noexcept {
        return *info_;
    }

private:
    friend class TypeRegistry;
    TypeBuilder(TypeRegistry& registry, TypeInfo& info) noexcept
        : registry_{&registry}, info_{&info} {}

    TypeRegistry* registry_;
    TypeInfo* info_;
};

// Kennt alle reflektierten Typen. Wird beim Start aufgebaut und danach nur noch gelesen; dann
// dürfen alle Threads ohne Sperre zugreifen. Grundtypen (bool, i8 bis i64, u8 bis u64, f32, f64,
// string) sind von Anfang an registriert. TypeInfo-Zeiger bleiben gültig, solange die Registry
// lebt.
class TypeRegistry {
public:
    TypeRegistry();
    ~TypeRegistry();
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;
    // TypeInfo-Zeiger bleiben beim Verschieben gültig. Eine Welt, die die Registry kennt, nicht.
    TypeRegistry(TypeRegistry&&) noexcept;
    TypeRegistry& operator=(TypeRegistry&&) noexcept;

    // Registriert T unter name. Name und C++-Typ dürfen noch nicht vergeben sein.
    template <Reflectable T>
    TypeBuilder<T> add(std::string_view name) {
        TypeInfo info{
            .name = std::string{name},
            .id = type_id(name),
            .size = sizeof(T),
            .alignment = alignof(T),
            .kind = kind_of<T>(),
            .trivial = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
            .ops = make_ops<T>(),
            .fields = {},
        };
        return TypeBuilder<T>{*this, add_type(type_key<T>(), std::move(info))};
    }

    template <typename T>
    [[nodiscard]] const TypeInfo* find() const noexcept {
        return find_key(type_key<T>());
    }
    // Eigener Name statt Überladung von find(): Ein Textliteral würde sonst als const void*
    // genommen statt als std::string_view.
    [[nodiscard]] const TypeInfo* find_key(TypeKey key) const noexcept;
    [[nodiscard]] const TypeInfo* find(std::string_view name) const noexcept;
    [[nodiscard]] const TypeInfo* find_id(TypeId id) const noexcept;

    // Wie find(), bricht aber ab, wenn T nicht registriert ist.
    template <typename T>
    [[nodiscard]] const TypeInfo& get() const {
        return get_key(type_key<T>());
    }
    [[nodiscard]] const TypeInfo& get_key(TypeKey key) const;

    // Alle Typen in der Reihenfolge der Registrierung.
    [[nodiscard]] std::span<const TypeInfo* const> types() const noexcept;

private:
    template <typename T>
    friend class TypeBuilder;

    TypeInfo& add_type(TypeKey key, TypeInfo info);
    void add_field(TypeInfo& owner, std::string_view name, TypeKey field_type, usize offset,
                   usize field_size);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace detail {

template <typename Member>
struct MemberTraits;

template <typename Class, typename M>
struct MemberTraits<M Class::*> {
    using ClassType = Class;
    using FieldType = M;
};

} // namespace detail

template <typename T>
template <typename Member>
    requires std::is_member_object_pointer_v<Member>
TypeBuilder<T>& TypeBuilder<T>::field(std::string_view name, Member member) {
    using Traits = detail::MemberTraits<Member>;
    static_assert(std::is_same_v<typename Traits::ClassType, T>,
                  "Das Feld gehört nicht zu diesem Typ");
    using M = typename Traits::FieldType;
    registry_->add_field(*info_, name, type_key<M>(), offset_of(member), sizeof(M));
    return *this;
}

} // namespace ngine::reflect
