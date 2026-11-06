module;

#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Inspector des Editors (EDIT-04): zeigt die Komponenten der ausgewählten Entity mit
// Eingabefeldern, die allein aus den Zeilen von edit::inspect() entstehen; kein Code pro
// Komponente. Änderungen gehen über edit::commit() als zusammenfassende Befehle; das
// Versiegeln des Verlaufs nach dem Loslassen übernimmt die Hauptschleife.
export module editor.inspector;

import ngine.core;
import ngine.edit;
import ngine.editor_ui;
import ngine.math;
import ngine.reflect;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

ui::DataType to_data_type(edit::Scalar scalar) {
    switch (scalar) {
    case edit::Scalar::I8: return ui::DataType::I8;
    case edit::Scalar::I16: return ui::DataType::I16;
    case edit::Scalar::I32: return ui::DataType::I32;
    case edit::Scalar::I64: return ui::DataType::I64;
    case edit::Scalar::U8: return ui::DataType::U8;
    case edit::Scalar::U16: return ui::DataType::U16;
    case edit::Scalar::U32: return ui::DataType::U32;
    case edit::Scalar::U64: return ui::DataType::U64;
    case edit::Scalar::F32: return ui::DataType::F32;
    case edit::Scalar::F64:
    case edit::Scalar::None: break;
    }
    return ui::DataType::F64;
}

bool is_float(edit::Scalar scalar) {
    return scalar == edit::Scalar::F32 || scalar == edit::Scalar::F64;
}

// Geschwindigkeit beim Ziehen pro Pixel.
f32 drag_speed(const edit::InspectorRow& row) {
    if (row.range) {
        if (row.range->step > 0.0) {
            return static_cast<f32>(row.range->step);
        }
        return static_cast<f32>((row.range->max - row.range->min) / 200.0);
    }
    return is_float(row.scalar) ? 0.05f : 1.0f;
}

// Wert eines Enums als Zahl; Enums sind im Speicher Ganzzahlen ihrer Größe.
i64 read_enum(const void* value, usize size) {
    switch (size) {
    case 1: {
        i8 v = 0;
        std::memcpy(&v, value, 1);
        return v;
    }
    case 2: {
        i16 v = 0;
        std::memcpy(&v, value, 2);
        return v;
    }
    case 4: {
        i32 v = 0;
        std::memcpy(&v, value, 4);
        return v;
    }
    default: {
        i64 v = 0;
        std::memcpy(&v, value, sizeof(v));
        return v;
    }
    }
}

void write_enum(void* value, usize size, i64 number) {
    switch (size) {
    case 1: {
        const auto v = static_cast<i8>(number);
        std::memcpy(value, &v, 1);
        break;
    }
    case 2: {
        const auto v = static_cast<i16>(number);
        std::memcpy(value, &v, 2);
        break;
    }
    case 4: {
        const auto v = static_cast<i32>(number);
        std::memcpy(value, &v, 4);
        break;
    }
    default: std::memcpy(value, &number, sizeof(number)); break;
    }
}

void report(const edit::Result<bool>& result) {
    if (!result) {
        log::error("{}", result.error());
    }
}

export class InspectorPanel {
public:
    void draw(edit::SceneDocument& document, edit::Selection& selection) {
        const edit::EntityId entity = selection.current();
        if (entity == 0 || !document.exists(entity)) {
            ui::text_disabled("Keine Entity ausgewählt");
            return;
        }
        ui::text_disabled(std::format("Entity {}", entity));

        ui::push_id(entity);
        for (const edit::ComponentRows& component : edit::inspect(document, entity)) {
            ui::push_id(component.type->name);
            const bool open = ui::collapsing_header(component.type->name);
            if (!component.read_only && ui::begin_popup_context_item("Komponente")) {
                if (ui::menu_item("Entfernen")) {
                    run(document, edit::remove_component(entity, component.type->id));
                }
                ui::end_popup();
            }
            if (open) {
                ui::begin_disabled(component.read_only);
                for (const edit::InspectorRow& row : component.rows) {
                    draw_row(document, entity, row);
                }
                ui::end_disabled();
                if (component.read_only) {
                    ui::text_disabled("Laufzeitkomponente, wird nicht gespeichert");
                }
            }
            ui::pop_id();
        }
        ui::pop_id();

        ui::separator();
        if (ui::button("Komponente hinzufügen")) {
            ui::open_popup("Hinzufügen");
        }
        if (ui::begin_popup("Hinzufügen")) {
            for (const reflect::TypeInfo* type : edit::addable_components(document, entity)) {
                if (ui::selectable(type->name)) {
                    run(document, edit::add_component(entity, type->id));
                }
            }
            ui::end_popup();
        }

        // Eulerwinkel nur während des Ziehens festhalten, sonst aus der Rotation lesen.
        if (!ui::is_any_item_active()) {
            rotation_.reset();
        }
    }

private:
    static void run(edit::SceneDocument& document, std::unique_ptr<edit::Command> command) {
        if (auto result = document.execute(std::move(command)); !result) {
            log::error("{}", result.error());
        }
    }

    void draw_row(edit::SceneDocument& document, edit::EntityId entity,
                  const edit::InspectorRow& row) {
        const void* value = edit::value_of(document, entity, row.path);
        if (value == nullptr && row.widget != edit::Widget::Group) {
            return;
        }
        const f32 indent = static_cast<f32>(row.depth) * 12.0f;
        if (indent > 0.0f) {
            ui::indent(indent);
        }
        ui::push_id(edit::to_string(document.types(), row.path));
        switch (row.widget) {
        case edit::Widget::Group: ui::text(row.label); break;
        case edit::Widget::Checkbox: {
            bool checked = *static_cast<const bool*>(value);
            if (ui::checkbox(row.label, checked)) {
                report(edit::commit(document, entity, row, &checked));
            }
            break;
        }
        case edit::Widget::Number:
        case edit::Widget::Vector: draw_numbers(document, entity, row, value); break;
        case edit::Widget::Color: {
            if (row.scalar != edit::Scalar::F32) {
                draw_numbers(document, entity, row, value);
                break;
            }
            std::array<f32, 4> color{};
            std::memcpy(color.data(), value, row.count * sizeof(f32));
            if (ui::color_edit(row.label, color.data(), row.count)) {
                report(edit::commit(document, entity, row, color.data()));
            }
            break;
        }
        case edit::Widget::Angle: draw_angle(document, entity, row, value); break;
        case edit::Widget::Rotation: draw_rotation(document, entity, row, value); break;
        case edit::Widget::Text: {
            std::string text = *static_cast<const std::string*>(value);
            if (ui::input_text(row.label, text)) {
                report(edit::commit(document, entity, row, &text));
            }
            break;
        }
        case edit::Widget::Enum: draw_enum(document, entity, row, value); break;
        case edit::Widget::Unsupported:
            ui::text_disabled(std::format("{} ({}, nicht bearbeitbar)", row.label,
                                          row.type != nullptr ? row.type->name : "?"));
            break;
        }
        ui::pop_id();
        if (indent > 0.0f) {
            ui::unindent(indent);
        }
    }

    static void draw_numbers(edit::SceneDocument& document, edit::EntityId entity,
                             const edit::InspectorRow& row, const void* value) {
        // Höchstens vier Zahlen zu acht Bytes.
        alignas(8) std::array<std::byte, 32> buffer{};
        const usize bytes = edit::scalar_size(row.scalar) * row.count;
        if (row.scalar == edit::Scalar::None || bytes > buffer.size() || row.type->size != bytes) {
            ui::text_disabled(row.label);
            return;
        }
        std::memcpy(buffer.data(), value, bytes);
        const f64* min = row.range ? &row.range->min : nullptr;
        const f64* max = row.range ? &row.range->max : nullptr;
        if (ui::drag_scalars(row.label, to_data_type(row.scalar), buffer.data(), row.count,
                             drag_speed(row), min, max, is_float(row.scalar) ? "%.3f" : "")) {
            report(edit::commit(document, entity, row, buffer.data()));
        }
    }

    // Winkel im Bogenmaß, angezeigt in Grad.
    static void draw_angle(edit::SceneDocument& document, edit::EntityId entity,
                           const edit::InspectorRow& row, const void* value) {
        f64 degrees = 0.0;
        if (row.scalar == edit::Scalar::F32) {
            degrees = static_cast<f64>(*static_cast<const f32*>(value)) * edit::degrees_per_radian;
        } else if (row.scalar == edit::Scalar::F64) {
            degrees = *static_cast<const f64*>(value) * edit::degrees_per_radian;
        } else {
            ui::text_disabled(row.label);
            return;
        }
        std::optional<f64> min;
        std::optional<f64> max;
        if (row.range) {
            min = row.range->min * edit::degrees_per_radian;
            max = row.range->max * edit::degrees_per_radian;
        }
        if (ui::drag_scalars(row.label, ui::DataType::F64, &degrees, 1, 0.5f, min ? &*min : nullptr,
                             max ? &*max : nullptr, "%.1f°")) {
            const f64 radians = degrees / edit::degrees_per_radian;
            if (row.scalar == edit::Scalar::F32) {
                const auto narrow = static_cast<f32>(radians);
                report(edit::commit(document, entity, row, &narrow));
            } else {
                report(edit::commit(document, entity, row, &radians));
            }
        }
    }

    void draw_rotation(edit::SceneDocument& document, edit::EntityId entity,
                       const edit::InspectorRow& row, const void* value) {
        const std::string key =
            std::format("{}/{}", entity, edit::to_string(document.types(), row.path));
        math::Vec3 degrees = edit::rotation_degrees(*static_cast<const math::Quat*>(value));
        if (rotation_ && rotation_->first == key) {
            degrees = rotation_->second;
        }
        // Keine „-0.0°“ anzeigen.
        for (f32* angle : {&degrees.x, &degrees.y, &degrees.z}) {
            if (*angle == 0.0f) {
                *angle = 0.0f;
            }
        }
        if (ui::drag_scalars(row.label, ui::DataType::F32, &degrees, 3, 0.5f, nullptr, nullptr,
                             "%.1f°")) {
            rotation_ = std::make_pair(key, degrees);
            report(edit::commit_rotation(document, entity, row, degrees));
        }
    }

    static void draw_enum(edit::SceneDocument& document, edit::EntityId entity,
                          const edit::InspectorRow& row, const void* value) {
        const reflect::TypeInfo& type = *row.type;
        std::vector<std::string> names;
        usize current = type.enumerators.size();
        for (usize i = 0; i < type.enumerators.size(); ++i) {
            names.push_back(type.enumerators[i].name);
            // Bytes vergleichen: so stimmt es auch für vorzeichenlose Enums.
            alignas(8) std::array<std::byte, 8> candidate{};
            write_enum(candidate.data(), type.size, type.enumerators[i].value);
            if (std::memcmp(candidate.data(), value, type.size) == 0) {
                current = i;
            }
        }
        if (current == type.enumerators.size()) {
            // Wert ohne Namen (z. B. aus einer Datei): als Zahl zeigen.
            names.push_back(std::format("{}", read_enum(value, type.size)));
        }
        if (ui::combo(row.label, current, names) && current < type.enumerators.size()) {
            alignas(8) std::array<std::byte, 8> buffer{};
            write_enum(buffer.data(), type.size, type.enumerators[current].value);
            report(edit::commit(document, entity, row, buffer.data()));
        }
    }

    // Eulerwinkel der Rotation, die gerade gezogen wird (Schlüssel: Entity und Feld). Aus dem
    // Quaternion zurückgerechnet würden sie bei 90° springen.
    std::optional<std::pair<std::string, math::Vec3>> rotation_;
};

} // namespace editor
