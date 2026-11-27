module;

#include <optional>
#include <span>
#include <string>
#include <vector>

export module ngine.replication:schema;

import ngine.core;
import ngine.reflect;

// Was repliziert wird (R5.2, ADR 0052): alle Komponenten, die ihre Registrierung mit
// .replicated() markiert, in der Reihenfolge der Registry. Server und Client bauen das Schema aus
// derselben Registrierung; ein Hash darüber steht in der Willkommensnachricht.
export namespace ngine::replication {

// 3: Befehle pro festem Schritt und Stand der eigenen Figur (R5.3).
// 4: Deltas des Terrains (GameMessage::Terrain, R5.4).
// 5: Ereignisse und Einmal-Klänge der Skripte (GameMessage::Event, GameMessage::Sound, R5.5).
inline constexpr u32 protocol_version = 5;

// Wie eine Komponente auf die Leitung kommt.
enum class Codec : u8 {
    // Jedes nicht lokale Feld ist ein Teil, geschrieben wie serialize::write_binary.
    Fields,
    // world::Transform quantisiert in drei Teilen: Position (Zelle zu 64 m und Millimeter),
    // Drehung (drei kleinste Komponenten mit je 15 Bit), Skalierung (drei f32).
    Transform,
};

// Eine replizierte Komponente. Teile sind die Einheit der Deltas: Ändert sich ein Teil, geht nur
// er über die Leitung.
struct SchemaComponent {
    const reflect::TypeInfo* type = nullptr;
    Codec codec = Codec::Fields;
    // Nur bei Codec::Fields: die replizierten Felder, je ein Teil.
    std::vector<const reflect::Field*> fields;
    // Höchstens 64.
    u32 parts = 0;
};

class Schema {
public:
    // Alle mit .replicated() markierten Typen der Registry (außer ersetzten). Die Registry muss
    // das Schema überleben.
    explicit Schema(const reflect::TypeRegistry& types);

    [[nodiscard]] std::span<const SchemaComponent> components() const noexcept {
        return components_;
    }
    [[nodiscard]] const SchemaComponent& component(usize index) const noexcept {
        return components_[index];
    }
    [[nodiscard]] usize size() const noexcept {
        return components_.size();
    }
    [[nodiscard]] std::optional<u16> index_of(const reflect::TypeInfo& type) const noexcept;
    // Index von world::Transform, wenn es repliziert wird (Interpolation, Relevanz).
    [[nodiscard]] std::optional<u16> transform_index() const noexcept {
        return transform_;
    }
    // FNV-1a über Protokoll, Namen, Felder und deren Typen.
    [[nodiscard]] u64 hash() const noexcept {
        return hash_;
    }
    // Namen der Komponenten mit Komma, für Meldungen.
    [[nodiscard]] std::string describe() const;

private:
    std::vector<SchemaComponent> components_;
    std::optional<u16> transform_;
    u64 hash_ = 0;
};

} // namespace ngine::replication
