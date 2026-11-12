module;

#include <algorithm>
#include <cmath>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module ngine.edit;

import ngine.core;
import ngine.math;
import ngine.world;

namespace ngine::edit {

namespace {

class TerrainEditCommand final : public Command {
public:
    TerrainEditCommand(EntityId terrain, voxel::TerrainEdit edit, std::string label)
        : terrain_{terrain}, edit_{std::move(edit)}, label_{std::move(label)} {}

    [[nodiscard]] std::string label() const override {
        return label_;
    }
    [[nodiscard]] Result<void> apply(SceneDocument& document) override {
        const u64 entity = document.entity_of(terrain_);
        if (entity == 0) {
            return std::unexpected(std::string{"Das Terrain gibt es nicht mehr"});
        }
        world::restore_terrain(document.world().entity(entity), edit_.changes, true);
        return {};
    }
    void revert(SceneDocument& document) override {
        if (const u64 entity = document.entity_of(terrain_); entity != 0) {
            world::restore_terrain(document.world().entity(entity), edit_.changes, false);
        }
    }
    [[nodiscard]] bool merge(const Command& next) override {
        const auto* other = dynamic_cast<const TerrainEditCommand*>(&next);
        if (other == nullptr || other->terrain_ != terrain_ || other->label_ != label_) {
            return false;
        }
        // Pro Chunk bleibt das erste „vorher“ und das letzte „nachher“.
        for (const voxel::DeltaChange& change : other->edit_.changes) {
            const auto it = std::find_if(
                edit_.changes.begin(), edit_.changes.end(),
                [&](const voxel::DeltaChange& own) { return own.coord == change.coord; });
            if (it != edit_.changes.end()) {
                it->after = change.after;
            } else {
                edit_.changes.push_back(change);
            }
        }
        return true;
    }

private:
    EntityId terrain_;
    voxel::TerrainEdit edit_;
    std::string label_;
};

} // namespace

std::string brush_label(voxel::BrushMode mode) {
    switch (mode) {
    case voxel::BrushMode::Add: return "Auftragen";
    case voxel::BrushMode::Remove: return "Abtragen";
    case voxel::BrushMode::Smooth: return "Glätten";
    case voxel::BrushMode::Flatten: return "Ebnen";
    case voxel::BrushMode::Paint: return "Bemalen";
    }
    return "Terrain ändern";
}

std::unique_ptr<Command> terrain_edit(EntityId terrain, voxel::TerrainEdit edit,
                                      std::string label) {
    return std::make_unique<TerrainEditCommand>(terrain, std::move(edit), std::move(label));
}

voxel::Brush TerrainBrushTool::brush(world::Entity terrain, math::DVec3 hit, f32 dt) const {
    const math::DVec3 center = world::terrain_voxels(terrain, hit);
    voxel::Brush brush{.mode = settings.mode,
                       .center = {static_cast<f32>(center.x), static_cast<f32>(center.y),
                                  static_cast<f32>(center.z)},
                       .radius =
                           std::max(settings.radius, 0.1f) / static_cast<f32>(voxel::voxel_size),
                       .material = settings.material,
                       .strength = 0.0f};
    // Gleiche Wirkung pro Sekunde bei jeder Bildrate: Anteil strength pro 0,1 s.
    const f32 strength = std::clamp(settings.strength, 0.0f, 1.0f);
    brush.strength = 1.0f - std::pow(1.0f - strength, std::clamp(dt, 0.0f, 0.1f) * 10.0f);
    if (settings.mode == voxel::BrushMode::Flatten && stroking_) {
        brush.center.y = static_cast<f32>(plane_y_);
    }
    return brush;
}

Result<bool> TerrainBrushTool::stroke(SceneDocument& document, EntityId terrain, math::DVec3 hit,
                                      f32 dt) {
    if (document.read_only()) {
        return std::unexpected(std::string{"Während des Spielens lässt sich nichts ändern"});
    }
    const u64 entity = document.entity_of(terrain);
    if (entity == 0 || !document.world().entity(entity).has<world::Terrain>()) {
        return std::unexpected(std::string{"Kein Terrain ausgewählt"});
    }
    const world::Entity target = document.world().entity(entity);
    const bool continues = stroking_ && terrain_ == terrain;
    if (!continues) {
        // Neuer Strich: eigener Schritt im Verlauf, Ebene auf der Höhe des ersten Treffers.
        document.history().seal();
        plane_y_ = world::terrain_voxels(target, hit).y;
        terrain_ = terrain;
        stroking_ = true;
    }
    voxel::TerrainEdit edit = world::edit_terrain(target, brush(target, hit, dt));
    if (edit.empty()) {
        return false;
    }
    // Immer mit Verschmelzen: Der erste Befehl nach seal() wird ein neuer Schritt, die weiteren
    // des Strichs gehen in ihm auf.
    if (auto result = document.execute(
            terrain_edit(terrain, std::move(edit), brush_label(settings.mode)), true);
        !result) {
        return std::unexpected(result.error());
    }
    return true;
}

} // namespace ngine::edit
