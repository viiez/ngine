module;

#include <memory>
#include <string>

export module ngine.edit:terrain;

import ngine.core;
import ngine.math;
import ngine.world;
import :document;

// Voxel-Werkzeuge des Editors ohne UI (EDIT-08, R3.4): Pinsel zum Formen und Bemalen eines
// Terrains als Befehle mit Undo. Ein Befehl hält die Deltas der betroffenen Chunks vorher und
// nachher (Bytes nach ADR 0042), nicht die Samples; ein Pinselstrich über viele Frames wird ein
// Schritt im Verlauf.
export namespace ngine::edit {

// Beschriftung im Verlauf, z. B. „Abtragen“.
[[nodiscard]] std::string brush_label(voxel::BrushMode mode);

// Befehl aus einer schon angewendeten Änderung (world::edit_terrain()). Ausführen setzt die
// Deltas nachher, Rücknehmen die vorher. Er verschmilzt mit dem nächsten Befehl desselben Pinsels
// am selben Terrain.
[[nodiscard]] std::unique_ptr<Command> terrain_edit(EntityId terrain, voxel::TerrainEdit edit,
                                                    std::string label);

// Einstellungen der Voxel-Werkzeuge. Längen in Metern.
struct TerrainBrushSettings {
    voxel::BrushMode mode = voxel::BrushMode::Remove;
    f32 radius = 2.0f;
    // Für Glätten und Ebnen: Anteil der Angleichung pro Sekunde, 0 bis 1.
    f32 strength = 0.5f;
    // Für Auftragen und Bemalen (Nummer in der Materialtabelle).
    voxel::Material material = voxel::rock;
};

// Ein Pinselstrich mit der Maus: stroke() in jedem Frame, in dem die Maus gedrückt über dem
// Terrain ist, end_stroke() beim Loslassen. Ebnen hält die Höhe vom Anfang des Strichs.
class TerrainBrushTool {
public:
    TerrainBrushSettings settings;

    // Pinsel an der Stelle hit (Weltkoordinaten) auf terrain; dt gleicht Glätten und Ebnen an die
    // Bildrate an. Liefert, ob sich etwas geändert hat. Fehler (Spielen, Terrain fehlt) lassen
    // alles, wie es ist.
    Result<bool> stroke(SceneDocument& document, EntityId terrain, math::DVec3 hit, f32 dt);
    void end_stroke() noexcept {
        stroking_ = false;
    }
    [[nodiscard]] bool stroking() const noexcept {
        return stroking_;
    }
    // Pinsel in Voxeln des Terrains, wie stroke() ihn anwenden würde.
    [[nodiscard]] voxel::Brush brush(world::Entity terrain, math::DVec3 hit, f32 dt) const;

private:
    bool stroking_ = false;
    EntityId terrain_ = 0;
    // Höhe der Ebene beim Ebnen, in Voxeln des Terrains.
    f64 plane_y_ = 0.0;
};

} // namespace ngine::edit
