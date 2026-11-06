module;

#include <algorithm>
#include <cmath>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

module ngine.edit;

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.world;

namespace ngine::edit {

namespace {

using reflect::Kind;
using reflect::TypeInfo;

// Struct aus 2 bis 4 Zahlen derselben Art ohne Lücken, z. B. Vec3: liefert die Art.
Scalar uniform_scalars(const TypeInfo& type) noexcept {
    if (type.kind != Kind::Struct || type.fields.size() < 2 || type.fields.size() > 4) {
        return Scalar::None;
    }
    const Scalar first = scalar_of(*type.fields.front().type);
    if (first == Scalar::None) {
        return Scalar::None;
    }
    const usize size = scalar_size(first);
    for (usize i = 0; i < type.fields.size(); ++i) {
        const reflect::Field& field = type.fields[i];
        if (scalar_of(*field.type) != first || field.offset != i * size) {
            return Scalar::None;
        }
    }
    return first;
}

void add_rows(const TypeInfo& type, FieldPath path, u32 depth, std::vector<InspectorRow>& rows) {
    for (u32 index = 0; index < type.fields.size(); ++index) {
        const reflect::Field& field = type.fields[index];
        const TypeInfo& field_type = *field.type;
        InspectorRow row{.path = path,
                         .label = field.name,
                         .depth = depth,
                         .type = &field_type,
                         .range = field.range};
        row.path.fields.push_back(index);

        const Scalar uniform = uniform_scalars(field_type);
        const auto count = static_cast<u32>(field_type.fields.size());
        if (field.hint == reflect::Hint::Rotation && uniform == Scalar::F32 && count == 4) {
            row.widget = Widget::Rotation;
            row.scalar = Scalar::F32;
            row.count = 3;
        } else if (field.hint == reflect::Hint::Color &&
                   (uniform == Scalar::F32 || uniform == Scalar::F64) && count >= 3) {
            row.widget = Widget::Color;
            row.scalar = uniform;
            row.count = count;
        } else if (uniform != Scalar::None) {
            row.widget = Widget::Vector;
            row.scalar = uniform;
            row.count = count;
            // Bereich der Elemente gilt für alle, wenn das Feld selbst keinen hat.
            if (!row.range && field_type.fields.front().range) {
                row.range = field_type.fields.front().range;
            }
        } else {
            switch (field_type.kind) {
            case Kind::Bool: row.widget = Widget::Checkbox; break;
            case Kind::Int:
            case Kind::UInt:
            case Kind::Float:
                row.scalar = scalar_of(field_type);
                row.widget = field.hint == reflect::Hint::Angle && field_type.kind == Kind::Float
                                 ? Widget::Angle
                                 : Widget::Number;
                break;
            case Kind::String: row.widget = Widget::Text; break;
            case Kind::Enum: row.widget = Widget::Enum; break;
            case Kind::Struct:
                row.widget = Widget::Group;
                rows.push_back(row);
                add_rows(field_type, row.path, depth + 1, rows);
                continue;
            }
        }
        rows.push_back(std::move(row));
    }
}

template <typename T>
void clamp_as(void* data, const reflect::Range& range) noexcept {
    T value;
    std::memcpy(&value, data, sizeof(T));
    const auto as_f64 = static_cast<f64>(value);
    // NaN bleibt NaN; Klemmen braucht vergleichbare Werte.
    if (as_f64 < range.min) {
        value = static_cast<T>(range.min);
    } else if (as_f64 > range.max) {
        value = static_cast<T>(range.max);
    }
    std::memcpy(data, &value, sizeof(T));
}

void clamp_scalar(Scalar scalar, void* data, const reflect::Range& range) noexcept {
    switch (scalar) {
    case Scalar::I8: clamp_as<i8>(data, range); break;
    case Scalar::I16: clamp_as<i16>(data, range); break;
    case Scalar::I32: clamp_as<i32>(data, range); break;
    case Scalar::I64: clamp_as<i64>(data, range); break;
    case Scalar::U8: clamp_as<u8>(data, range); break;
    case Scalar::U16: clamp_as<u16>(data, range); break;
    case Scalar::U32: clamp_as<u32>(data, range); break;
    case Scalar::U64: clamp_as<u64>(data, range); break;
    case Scalar::F32: clamp_as<f32>(data, range); break;
    case Scalar::F64: clamp_as<f64>(data, range); break;
    case Scalar::None: break;
    }
}

} // namespace

Scalar scalar_of(const reflect::TypeInfo& type) noexcept {
    switch (type.kind) {
    case Kind::Int:
        switch (type.size) {
        case 1: return Scalar::I8;
        case 2: return Scalar::I16;
        case 4: return Scalar::I32;
        default: return Scalar::I64;
        }
    case Kind::UInt:
        switch (type.size) {
        case 1: return Scalar::U8;
        case 2: return Scalar::U16;
        case 4: return Scalar::U32;
        default: return Scalar::U64;
        }
    case Kind::Float: return type.size == 4 ? Scalar::F32 : Scalar::F64;
    default: return Scalar::None;
    }
}

usize scalar_size(Scalar scalar) noexcept {
    switch (scalar) {
    case Scalar::I8:
    case Scalar::U8: return 1;
    case Scalar::I16:
    case Scalar::U16: return 2;
    case Scalar::I32:
    case Scalar::U32:
    case Scalar::F32: return 4;
    case Scalar::I64:
    case Scalar::U64:
    case Scalar::F64: return 8;
    case Scalar::None: break;
    }
    return 0;
}

std::vector<InspectorRow> build_rows(const reflect::TypeInfo& component) {
    std::vector<InspectorRow> rows;
    add_rows(component, FieldPath{.component = component.id, .fields = {}}, 0, rows);
    return rows;
}

std::vector<ComponentRows> inspect(SceneDocument& document, EntityId entity) {
    std::vector<ComponentRows> result;
    const u64 world_entity = document.entity_of(entity);
    if (world_entity == 0) {
        return result;
    }
    std::vector<const TypeInfo*> types;
    auto visit = [&](const TypeInfo& type, world::ComponentId /*component*/, const void* /*data*/) {
        if (!type.fields.empty()) {
            types.push_back(&type);
        }
    };
    document.world().for_each_component(
        world_entity, world::ComponentVisitor{
                          .context = &visit,
                          .visit =
                              [](void* context, const TypeInfo& type, world::ComponentId component,
                                 const void* data) {
                                  (*static_cast<decltype(visit)*>(context))(type, component, data);
                              },
                      });
    std::ranges::sort(types, {}, &TypeInfo::index);
    for (const TypeInfo* type : types) {
        result.push_back(
            ComponentRows{.type = type, .read_only = type->transient, .rows = build_rows(*type)});
    }
    return result;
}

std::vector<const reflect::TypeInfo*> addable_components(SceneDocument& document, EntityId entity) {
    std::vector<const TypeInfo*> result;
    const u64 world_entity = document.entity_of(entity);
    if (world_entity == 0) {
        return result;
    }
    const std::span<const TypeInfo* const> types = document.types().types();
    // Typen, die Feld eines anderen Structs sind (Vec3, Quat …), gelten als Werte, nicht als
    // Komponenten. Die Reflection unterscheidet beides nicht (ADR 0018).
    std::vector<bool> value_type(types.size(), false);
    for (const TypeInfo* type : types) {
        for (const reflect::Field& field : type->fields) {
            value_type[field.type->index] = true;
        }
    }
    world::World& world = document.world();
    for (const TypeInfo* type : types) {
        if (type->kind != Kind::Struct || type->transient || type->fields.empty() ||
            value_type[type->index]) {
            continue;
        }
        if (world.has(world_entity, world.component(*type))) {
            continue;
        }
        result.push_back(type);
    }
    return result;
}

const void* value_of(SceneDocument& document, EntityId entity, const FieldPath& path) {
    const u64 world_entity = document.entity_of(entity);
    auto resolved = resolve(document.types(), path);
    if (world_entity == 0 || !resolved) {
        return nullptr;
    }
    world::World& world = document.world();
    const world::ComponentId component = world.component(*resolved->component);
    if (!world.has(world_entity, component)) {
        return nullptr;
    }
    return static_cast<const std::byte*>(world.get(world_entity, component)) + resolved->offset;
}

void clamp_to_range(const InspectorRow& row, void* value) {
    if (!row.range || row.scalar == Scalar::None) {
        return;
    }
    const usize size = scalar_size(row.scalar);
    for (u32 i = 0; i < row.count; ++i) {
        clamp_scalar(row.scalar, static_cast<std::byte*>(value) + i * size, *row.range);
    }
}

Result<bool> commit(SceneDocument& document, EntityId entity, const InspectorRow& row,
                    const void* value) {
    const reflect::TypeInfo& type = *row.type;
    // Kopie, damit das Klemmen den Wert des Aufrufers nicht ändert.
    auto storage = std::make_unique<std::byte[]>(type.size);
    void* copy = storage.get();
    type.ops.copy_construct(copy, value);
    if (row.widget == Widget::Number || row.widget == Widget::Angle ||
        row.widget == Widget::Vector || row.widget == Widget::Color) {
        clamp_to_range(row, copy);
    }
    auto command = set_field(document, entity, row.path, copy);
    type.ops.destroy(copy);
    if (!command) {
        return std::unexpected(command.error());
    }
    if (*command == nullptr) {
        return false;
    }
    if (auto result = document.execute(*std::move(command), true); !result) {
        return std::unexpected(result.error());
    }
    return true;
}

math::Vec3 rotation_degrees(const math::Quat& rotation) noexcept {
    return math::to_euler(rotation) * static_cast<f32>(degrees_per_radian);
}

Result<bool> commit_rotation(SceneDocument& document, EntityId entity, const InspectorRow& row,
                             math::Vec3 degrees) {
    verify(row.widget == Widget::Rotation, "commit_rotation() für eine Zeile ohne Drehung");
    const math::Quat rotation =
        math::normalize(math::from_euler(degrees * static_cast<f32>(1.0 / degrees_per_radian)));
    return commit(document, entity, row, &rotation);
}

} // namespace ngine::edit
