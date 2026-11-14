module;

#include <format>
#include <string>
#include <utility>

// Fenster „Terrain-Materialien“ des Editors (VOX-07, R4.3): Liste der Materialien der Tabelle
// terrain/standard.terrain-materials, Hinzufügen und Entfernen am Ende (die Reihenfolge ist die
// Materialnummer der Voxel) und die Felder des gewählten Materials. Jede Änderung gilt sofort in
// Renderer und Physik; „Speichern“ schreibt die Datei. Texturen kommen per Ziehen aus dem
// Asset-Browser.
export module editor.terrain_materials;

import ngine.core;
import ngine.asset;
import ngine.editor_ui;
import ngine.world;
import app.model_scene;
import app.terrain_materials;
import editor.assets;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

export constexpr const char* terrain_materials_window = "Terrain-Materialien";

export class TerrainMaterialsPanel {
public:
    void draw(app::ModelScene& scene, const asset::AssetDatabase* database, bool& open) {
        if (!open) {
            return;
        }
        app::TerrainMaterialLibrary& library = scene.terrain_materials();
        if (ui::begin_window(terrain_materials_window, &open)) {
            // Arbeitskopie; jede Änderung geht sofort an die Bibliothek.
            world::TerrainMaterials table = library.table();
            bool changed = false;
            if (selected_ >= table.materials.size()) {
                selected_ = table.materials.size() > 1 ? 1 : 0;
            }
            for (usize i = 1; i < table.materials.size(); ++i) {
                ui::push_id(i);
                if (ui::selectable(std::format("{}  {}", i, table.materials[i].name),
                                   i == selected_)) {
                    selected_ = i;
                }
                ui::pop_id();
            }
            if (ui::button("Hinzufügen") && table.materials.size() < world::max_terrain_materials) {
                world::TerrainMaterial material;
                material.name = std::format("Material {}", table.materials.size());
                table.materials.push_back(std::move(material));
                selected_ = table.materials.size() - 1;
                changed = true;
            }
            ui::same_line();
            // Nur das letzte, damit die Nummern der anderen bleiben.
            ui::begin_disabled(table.materials.size() <= 1 ||
                               selected_ != table.materials.size() - 1);
            if (ui::button("Letztes entfernen")) {
                table.materials.pop_back();
                selected_ = table.materials.size() - 1;
                changed = true;
            }
            ui::end_disabled();
            ui::same_line();
            ui::begin_disabled(library.path().empty());
            if (ui::button("Speichern")) {
                if (auto saved = world::save_terrain_materials_file(table, library.path())) {
                    log::info("Terrain-Materialien gespeichert: {}", library.path());
                    dirty_ = false;
                } else {
                    log::error("Terrain-Materialien: {}", saved.error());
                }
            }
            ui::end_disabled();
            if (dirty_) {
                ui::same_line();
                ui::text_disabled("(geändert)");
            }
            ui::separator();
            if (selected_ > 0 && selected_ < table.materials.size()) {
                changed = draw_material(table.materials[selected_], database) || changed;
            }
            if (changed) {
                library.set(std::move(table));
                scene.apply_terrain_materials();
                dirty_ = true;
            }
        }
        ui::end_window();
    }

private:
    static bool draw_material(world::TerrainMaterial& material,
                              const asset::AssetDatabase* database) {
        bool changed = false;
        changed = ui::input_text("Name", material.name) || changed;
        changed = ui::color_edit("Farbe", &material.color.x, 3) || changed;
        const auto number = [&](const char* label, f32& value, f64 min, f64 max, f32 speed) {
            changed =
                ui::drag_scalars(label, ui::DataType::F32, &value, 1, speed, &min, &max, "%.2f") ||
                changed;
        };
        number("Roughness", material.roughness, 0.0, 1.0, 0.01f);
        number("Maßstab (m)", material.scale, 0.1, 100.0, 0.05f);
        number("Normalen", material.normal_scale, 0.0, 4.0, 0.01f);
        changed = texture("Grundfarbe", material.albedo, database) || changed;
        changed = texture("Normalen-Textur", material.normal, database) || changed;
        number("Reibung", material.friction, 0.0, 2.0, 0.01f);
        number("Rückprall", material.restitution, 0.0, 1.0, 0.01f);
        changed = ui::input_text("Schritte", material.footsteps) || changed;
        return changed;
    }

    // Textur als Pfad; Ablegen aus dem Asset-Browser setzt sie, „x“ entfernt sie.
    static bool texture(const char* label, u64& id, const asset::AssetDatabase* database) {
        bool changed = false;
        const asset::AssetInfo* info =
            database != nullptr && id != 0 ? database->find(id) : nullptr;
        ui::push_id(label);
        ui::selectable(std::format("{}: {}", label,
                                   info != nullptr ? info->path
                                   : id != 0       ? std::format("{} (fehlt)", id)
                                                   : std::string{"keine"}));
        if (const auto dropped = ui::drop_target(asset_payload)) {
            const asset::AssetInfo* target =
                database != nullptr ? database->find(*dropped) : nullptr;
            if (target != nullptr && target->type == asset::AssetType::Texture) {
                id = target->id;
                changed = true;
            }
        }
        if (id != 0) {
            ui::same_line();
            if (ui::button("x")) {
                id = 0;
                changed = true;
            }
        }
        ui::pop_id();
        return changed;
    }

    usize selected_ = 1;
    bool dirty_ = false;
};

} // namespace editor
