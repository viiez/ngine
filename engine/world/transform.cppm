export module ngine.world:transform;

import ngine.core;
import ngine.math;
import :ecs;

export namespace ngine::world {

// Lage einer Entity relativ zu ihrem Elternteil (ohne Elternteil: relativ zur Welt). Die Position
// ist 64 Bit (WORLD-06, ADR 0012), Drehung und Skalierung 32 Bit.
struct Transform {
    math::DVec3 position{0.0, 0.0, 0.0};
    math::Quat rotation = math::quat_identity();
    math::Vec3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] math::Affine local() const noexcept {
        return math::Affine::from(position, rotation, scale);
    }
};

// Lage in der Welt, berechnet von update_transforms() aus Transform und Hierarchie. Jede Entity
// mit Transform bekommt sie automatisch; gespeichert wird sie nicht.
struct WorldTransform {
    math::Affine value;

    [[nodiscard]] math::DVec3 position() const noexcept {
        return value.translation;
    }
};

// Berechnet WorldTransform aus Transform und Hierarchie, Eltern vor Kindern (WORLD-03). Ein
// Vorfahr ohne Transform zählt wie keine Verschiebung; die Kette geht beim nächsten Vorfahren mit
// Transform weiter. Läuft wie andere Strukturarbeit nach Schedule::run().
//
// Seit R2.3 nur für geänderte Teilbäume (TD-15): neu berechnet werden Entities, deren Transform
// sich seit dem letzten Aufruf geändert hat, die neu oder umgehängt sind, und alle ihre
// Nachfahren. Änderungen über Entity::get_mut() brauchen dafür Entity::modified(). Liefert die
// Zahl der neu berechneten Entities (die Erkennung arbeitet pro Chunk, es können also mehr sein
// als geändert wurden).
usize update_transforms(World& world);

// Weltlage einer Entity, direkt aus den Transforms ihrer Vorfahren berechnet; unabhängig davon,
// ob WorldTransform aktuell ist. Für Werkzeuge wie Gizmos und Umhängen im Editor, die einzelne
// Entities brauchen. Ohne Transform zählt eine Entity wie keine Verschiebung.
[[nodiscard]] math::Affine world_affine(World& world, u64 entity);

} // namespace ngine::world
