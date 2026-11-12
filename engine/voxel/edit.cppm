module;

#include <optional>

export module ngine.voxel:edit;

import ngine.core;
import :grid;
import :chunk;

export namespace ngine::voxel {

enum class BrushMode : u8 {
    // Kugel aus festem Stoff hinzufügen.
    Add,
    // Kugel ausschneiden.
    Remove,
    // Dichte mit den Nachbarn mitteln; rundet Kanten ab.
    Smooth,
    // Zur waagrechten Ebene durch die Mitte hin angleichen: füllt darunter auf, trägt darüber ab
    // (R3.4).
    Flatten,
    // Material fester Samples ersetzen, ohne die Form zu ändern (R3.4).
    Paint,
};

// Kugelförmiger Pinsel. Mitte und Radius in Voxeln (Gitterkoordinaten).
struct Brush {
    BrushMode mode = BrushMode::Remove;
    Vec3 center;
    f32 radius = 3.0f;
    // Für Add und Paint: Material der neuen bzw. bemalten Samples; Smooth und Flatten nehmen es
    // für fest gewordene Samples ohne festen Nachbarn.
    Material material = 2;
    // Für Smooth und Flatten: Anteil der Angleichung pro Anwendung, 0 bis 1.
    f32 strength = 0.5f;
};

// Bereich der Samples, die der Pinsel ändern kann (Koordinaten des Terrains, ungeklemmt). Glätten
// liest zusätzlich die Nachbarn im Abstand 1.
[[nodiscard]] SampleBox brush_box(const Brush& brush);

// Wendet den Pinsel an und liefert den Bereich der Samples, der sich geändert haben kann. Erhöht
// die Version des Gitters.
Box apply(Grid& grid, const Brush& brush);

// Wendet den Pinsel auf alle Chunks an, die er berührt (Mitte und Radius in Voxeln relativ zum
// Ursprung des Terrains). Legt fehlende Chunks beim Auftragen an, schrumpft einheitlich
// gewordene und merkt die betroffenen Netze vor (auch die der Nachbarn, VOX-03). Liefert den
// geänderten Bereich, leer, wenn sich nichts geändert hat.
SampleBox apply(ChunkStore& store, const Brush& brush);

struct RayHit {
    Vec3 position;
    f32 distance = 0.0f;
};

// Sucht entlang eines Strahls (Gitterkoordinaten, direction normiert) die erste Oberfläche. Läuft
// in Schritten von einem Viertel Voxel und verfeinert den Treffer linear.
[[nodiscard]] std::optional<RayHit> raycast(const Grid& grid, Vec3 origin, Vec3 direction,
                                            f32 max_length);
// Wie oben über alle Chunks; Koordinaten in Voxeln relativ zum Ursprung des Terrains. Läuft über
// Chunk-Grenzen hinweg, außerhalb der Chunks ist Luft.
[[nodiscard]] std::optional<RayHit> raycast(const ChunkStore& store, Vec3 origin, Vec3 direction,
                                            f32 max_length);

} // namespace ngine::voxel
