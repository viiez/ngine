module;

#include <deque>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

module ngine.reflect;

import ngine.core;

namespace ngine::reflect {

struct TypeRegistry::Impl {
    // std::deque verschiebt seine Elemente beim Wachsen nicht; TypeInfo-Zeiger bleiben gültig.
    std::deque<TypeInfo> storage;
    std::vector<const TypeInfo*> ordered;
    std::unordered_map<TypeKey, const TypeInfo*> by_key;
    std::unordered_map<TypeId, const TypeInfo*> by_id;
};

TypeRegistry::TypeRegistry() : impl_{std::make_unique<Impl>()} {
    add<bool>("bool");
    add<i8>("i8");
    add<i16>("i16");
    add<i32>("i32");
    add<i64>("i64");
    add<u8>("u8");
    add<u16>("u16");
    add<u32>("u32");
    add<u64>("u64");
    add<f32>("f32");
    add<f64>("f64");
    add<std::string>("string");
}

TypeRegistry::~TypeRegistry() = default;
TypeRegistry::TypeRegistry(TypeRegistry&&) noexcept = default;
TypeRegistry& TypeRegistry::operator=(TypeRegistry&&) noexcept = default;

TypeInfo& TypeRegistry::add_type(TypeKey key, TypeInfo info) {
    if (impl_->by_key.contains(key)) {
        log::fatal("Reflection: Typ '{}' ist schon registriert", info.name);
        verify(false, "Typ doppelt registriert");
    }
    if (const auto it = impl_->by_id.find(info.id); it != impl_->by_id.end()) {
        log::fatal("Reflection: Name '{}' ist schon vergeben (oder hat denselben Hash wie '{}')",
                   info.name, it->second->name);
        verify(false, "Typname doppelt vergeben");
    }
    TypeInfo& stored = impl_->storage.emplace_back(std::move(info));
    impl_->ordered.push_back(&stored);
    impl_->by_key.emplace(key, &stored);
    impl_->by_id.emplace(stored.id, &stored);
    return stored;
}

void TypeRegistry::add_field(TypeInfo& owner, std::string_view name, TypeKey field_type,
                             usize offset, usize field_size) {
    const TypeInfo* type = find_key(field_type);
    if (type == nullptr) {
        log::fatal("Reflection: Feld '{}.{}' hat einen Typ, der nicht registriert ist", owner.name,
                   name);
        verify(false, "Feldtyp nicht registriert");
    }
    if (owner.find_field(name) != nullptr) {
        log::fatal("Reflection: Feld '{}.{}' gibt es schon", owner.name, name);
        verify(false, "Feld doppelt registriert");
    }
    verify(type->size == field_size && offset + field_size <= owner.size,
           "Feld passt nicht in seinen Typ");
    owner.fields.push_back(Field{.name = std::string{name}, .type = type, .offset = offset});
}

const TypeInfo* TypeRegistry::find_key(TypeKey key) const noexcept {
    const auto it = impl_->by_key.find(key);
    return it != impl_->by_key.end() ? it->second : nullptr;
}

const TypeInfo* TypeRegistry::find(std::string_view name) const noexcept {
    const TypeInfo* type = find_id(type_id(name));
    // Gleicher Hash heißt nicht sicher gleicher Name.
    return type != nullptr && type->name == name ? type : nullptr;
}

const TypeInfo* TypeRegistry::find_id(TypeId id) const noexcept {
    const auto it = impl_->by_id.find(id);
    return it != impl_->by_id.end() ? it->second : nullptr;
}

const TypeInfo& TypeRegistry::get_key(TypeKey key) const {
    const TypeInfo* type = find_key(key);
    verify(type != nullptr, "Typ ist nicht in der Reflection registriert");
    return *type;
}

std::span<const TypeInfo* const> TypeRegistry::types() const noexcept {
    return impl_->ordered;
}

} // namespace ngine::reflect
