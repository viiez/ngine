module;

#include <algorithm>
#include <cstddef>
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

// Vorlage eines zusammengesetzten Typs (Speicher und die Felder, mit denen er gebaut wurde).
struct CompositeData {
    std::unique_ptr<std::byte[]> storage;
    std::byte* prototype = nullptr;
    std::vector<Field> fields;

    CompositeData() = default;
    CompositeData(const CompositeData&) = delete;
    CompositeData& operator=(const CompositeData&) = delete;
    ~CompositeData() {
        if (prototype != nullptr) {
            for (auto field = fields.rbegin(); field != fields.rend(); ++field) {
                field->type->destroy(prototype + field->offset);
            }
        }
    }
};

namespace {

std::byte* at(void* base, usize offset) {
    return static_cast<std::byte*>(base) + offset;
}
const std::byte* at(const void* base, usize offset) {
    return static_cast<const std::byte*>(base) + offset;
}

// Lebenszyklus zusammengesetzter Typen: Feld für Feld über die Feldtypen.
void composite_copy_construct(const TypeInfo& type, void* target, const void* source) {
    for (const Field& field : type.fields) {
        field.type->copy_construct(at(target, field.offset), at(source, field.offset));
    }
}

const TypeOps composite_ops{
    .construct =
        [](const TypeInfo& type, void* target) {
            composite_copy_construct(type, target, type.composite->prototype);
        },
    .destroy =
        [](const TypeInfo& type, void* target) noexcept {
            for (auto field = type.fields.rbegin(); field != type.fields.rend(); ++field) {
                field->type->destroy(at(target, field->offset));
            }
        },
    .copy_construct = composite_copy_construct,
    .move_construct =
        [](const TypeInfo& type, void* target, void* source) noexcept {
            for (const Field& field : type.fields) {
                field.type->move_construct(at(target, field.offset), at(source, field.offset));
            }
        },
    .copy_assign =
        [](const TypeInfo& type, void* target, const void* source) {
            for (const Field& field : type.fields) {
                field.type->copy_assign(at(target, field.offset), at(source, field.offset));
            }
        },
    .move_assign =
        [](const TypeInfo& type, void* target, void* source) noexcept {
            for (const Field& field : type.fields) {
                field.type->move_assign(at(target, field.offset), at(source, field.offset));
            }
        },
};

} // namespace

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
    info.index = static_cast<u32>(impl_->ordered.size());
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

void TypeRegistry::set_range(TypeInfo& owner, Range range) {
    verify(!owner.fields.empty(), "range() vor dem ersten Feld");
    Field& field = owner.fields.back();
    const Kind kind = field.type->kind;
    if (kind != Kind::Int && kind != Kind::UInt && kind != Kind::Float) {
        log::fatal("Reflection: Bereich für '{}.{}', das keine Zahl ist", owner.name, field.name);
        verify(false, "Bereich für ein Feld ohne Zahl");
    }
    verify(range.min <= range.max && range.step >= 0.0, "Ungültiger Bereich");
    field.range = range;
}

void TypeRegistry::set_hint(TypeInfo& owner, Hint hint) {
    verify(!owner.fields.empty(), "hint() vor dem ersten Feld");
    owner.fields.back().hint = hint;
}

void TypeRegistry::add_enumerator(TypeInfo& owner, std::string_view name, i64 value) {
    verify(owner.kind == Kind::Enum, "value() nur für Enums");
    if (owner.find_enumerator(name) != nullptr || owner.find_enumerator(value) != nullptr) {
        log::fatal("Reflection: Wert '{}' von '{}' ist doppelt", name, owner.name);
        verify(false, "Enum-Wert doppelt");
    }
    owner.enumerators.push_back(Enumerator{.name = std::string{name}, .value = value});
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

TypeInfo& TypeRegistry::add_composite(std::string_view name,
                                      std::span<const CompositeField> fields) {
    auto data = std::make_shared<CompositeData>();
    TypeInfo info{.name = std::string{name},
                  .id = type_id(name),
                  .size = 0,
                  .alignment = 1,
                  .kind = Kind::Struct,
                  .trivial = true,
                  .ops = composite_ops,
                  .fields = {}};
    usize offset = 0;
    for (const CompositeField& field : fields) {
        verify(field.type != nullptr && field.type->alignment > 0,
               "Feld eines zusammengesetzten Typs ohne Typ");
        if (info.find_field(field.name) != nullptr) {
            log::fatal("Reflection: Feld '{}.{}' gibt es schon", name, field.name);
            verify(false, "Feld doppelt registriert");
        }
        offset =
            (offset + field.type->alignment - 1) / field.type->alignment * field.type->alignment;
        info.fields.push_back(Field{.name = field.name,
                                    .type = field.type,
                                    .offset = offset,
                                    .range = field.range,
                                    .hint = field.hint});
        offset += field.type->size;
        info.alignment = std::max(info.alignment, field.type->alignment);
        info.trivial = info.trivial && field.type->trivial;
    }
    info.size = std::max<usize>((offset + info.alignment - 1) / info.alignment * info.alignment, 1);
    verify(info.alignment <= alignof(std::max_align_t),
           "Zusammengesetzter Typ: Ausrichtung zu groß");

    // Vorlage: jedes Feld mit dem Standardwert seines Typs.
    data->storage = std::make_unique<std::byte[]>(info.size);
    data->fields = info.fields;
    for (const Field& field : info.fields) {
        field.type->construct(data->storage.get() + field.offset);
    }
    data->prototype = data->storage.get();
    info.composite = data;
    // Schlüssel: die Vorlage, eindeutig pro zusammengesetztem Typ.
    return add_type(data.get(), std::move(info));
}

void* TypeRegistry::composite_prototype(const TypeInfo& type) {
    verify(type.composite != nullptr, "Kein zusammengesetzter Typ");
    return type.composite->prototype;
}

void TypeRegistry::retire(const TypeInfo& type) {
    verify(find_id(type.id) == &type, "Typ ist nicht (mehr) unter seinem Namen registriert");
    impl_->by_id.erase(type.id);
    impl_->storage[type.index].retired = true;
}

} // namespace ngine::reflect
