module;

#include <optional>

export module ngine.voxel:edit;

import ngine.core;
import :grid;

export namespace ngine::voxel {

enum class BrushMode : u8 {
    // Kugel aus festem Stoff hinzufügen.
    Add,
    // Kugel ausschneiden.
    Remove,
    // Dichte mit den Nachbarn mitteln; rundet Kanten ab.
    Smooth,
};

// Kugelförmiger Pinsel. Mitte und Radius in Voxeln (Gitterkoordinaten).
struct Brush {
    BrushMode mode = BrushMode::Remove;
    Vec3 center;
    f32 radius = 3.0f;
    // Nur für Add: Material der neuen Samples.
    Material material = 2;
    // Nur für Smooth: Anteil der Mittelung pro Anwendung, 0 bis 1.
    f32 strength = 0.5f;
};

// Wendet den Pinsel an und liefert den Bereich der Samples, der sich geändert haben kann. Erhöht
// die Version des Gitters.
Box apply(Grid& grid, const Brush& brush);

struct RayHit {
    Vec3 position;
    f32 distance = 0.0f;
};

// Sucht entlang eines Strahls (Gitterkoordinaten, direction normiert) die erste Oberfläche. Läuft
// in Schritten von einem Viertel Voxel und verfeinert den Treffer linear.
[[nodiscard]] std::optional<RayHit> raycast(const Grid& grid, Vec3 origin, Vec3 direction,
                                            f32 max_length);

} // namespace ngine::voxel
