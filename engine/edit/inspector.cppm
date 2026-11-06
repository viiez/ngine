module;

#include <optional>
#include <string>
#include <vector>

export module ngine.edit:inspector;

import ngine.core;
import ngine.math;
import ngine.reflect;
import :document;
import :commands;

// Inspector-Modell (EDIT-04): Zeilen mit Eingabefeldern allein aus der Reflection, ohne Code pro
// Komponente. Die Zeilen eines Typs hängen nicht von Werten ab; die Werte liest das Fenster in
// jedem Frame über value_of() und gibt neue über commit() zurück. commit() klemmt auf den Bereich
// und führt einen Befehl aus, den aufeinanderfolgende Änderungen desselben Feldes erweitern, bis
// das Fenster den Verlauf versiegelt (CommandStack::seal(), z. B. beim Loslassen eines Reglers).
export namespace ngine::edit {

// Eingabefeld einer Zeile.
enum class Widget : u8 {
    // Überschrift eines verschachtelten Structs; darunter seine Felder mit größerer Tiefe.
    Group,
    Checkbox,
    // Eine Zahl (scalar sagt welche).
    Number,
    // 2 bis 4 Zahlen gleicher Art nebeneinander (Vec3, DVec3 …).
    Vector,
    // 3 oder 4 Gleitkommazahlen als lineare Farbe (Hint::Color).
    Color,
    // Quaternion als Eulerwinkel in Grad (Hint::Rotation).
    Rotation,
    // f32 oder f64 im Bogenmaß, angezeigt in Grad (Hint::Angle).
    Angle,
    Text,
    // Auswahl unter den Namen eines Enums.
    Enum,
    // Typ ohne passendes Eingabefeld.
    Unsupported,
};

// Art einer Zahl im Speicher.
enum class Scalar : u8 { None, I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

[[nodiscard]] Scalar scalar_of(const reflect::TypeInfo& type) noexcept;
[[nodiscard]] usize scalar_size(Scalar scalar) noexcept;

struct InspectorRow {
    FieldPath path;
    std::string label;
    u32 depth = 0;
    Widget widget = Widget::Unsupported;
    // Typ des Feldes.
    const reflect::TypeInfo* type = nullptr;
    // Bei Number, Vector, Color und Angle: Art der Zahlen; count ist ihre Anzahl.
    Scalar scalar = Scalar::None;
    u32 count = 1;
    std::optional<reflect::Range> range;
};

// Zeilen eines Komponententyps, ohne die Überschrift der Komponente selbst.
[[nodiscard]] std::vector<InspectorRow> build_rows(const reflect::TypeInfo& component);

struct ComponentRows {
    const reflect::TypeInfo* type = nullptr;
    // Laufzeitkomponenten (TypeInfo::transient) zeigt der Inspector nur an.
    bool read_only = false;
    std::vector<InspectorRow> rows;
};

// Reflektierte Komponenten einer Entity in der Reihenfolge der Registrierung, ohne solche ohne
// Felder (z. B. WorldTransform).
[[nodiscard]] std::vector<ComponentRows> inspect(SceneDocument& document, EntityId entity);

// Komponententypen, die entity noch nicht hat und die sich speichern lassen. Structs, die als Feld
// eines anderen Typs vorkommen (Vec3, Quat …), zählen als Werte und fehlen.
[[nodiscard]] std::vector<const reflect::TypeInfo*> addable_components(SceneDocument& document,
                                                                       EntityId entity);

// Aktueller Wert eines Feldes (nur lesen) oder nullptr, wenn es Entity oder Komponente nicht gibt.
// Gültig bis zur nächsten Änderung der Welt.
[[nodiscard]] const void* value_of(SceneDocument& document, EntityId entity, const FieldPath& path);

// Neuer Wert für row (zeigt auf einen Wert des Feldtyps): Zahlen werden auf den Bereich geklemmt,
// dann läuft ein zusammenfassender Befehl. Liefert true, wenn sich etwas geändert hat.
Result<bool> commit(SceneDocument& document, EntityId entity, const InspectorRow& row,
                    const void* value);

// Klemmt die Zahlen von row in value auf den Bereich; ohne Bereich nichts.
void clamp_to_range(const InspectorRow& row, void* value);

// Darstellung von Drehungen und Winkeln.
inline constexpr f64 degrees_per_radian = 57.29577951308232;
// Eulerwinkel in Grad (x pitch, y yaw, z roll) eines Quat.
[[nodiscard]] math::Vec3 rotation_degrees(const math::Quat& rotation) noexcept;
// Setzt eine Rotation-Zeile auf Eulerwinkel in Grad.
Result<bool> commit_rotation(SceneDocument& document, EntityId entity, const InspectorRow& row,
                             math::Vec3 degrees);

} // namespace ngine::edit
