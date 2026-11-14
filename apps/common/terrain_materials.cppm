module;

#include <optional>
#include <string>
#include <vector>

// Materialtabelle des Terrains für Sandbox und Editor (R4.3): terrain/standard.terrain-materials
// aus dem Asset-Ordner, im laufenden Betrieb neu geladen, wenn sich die Datei ändert (auch wenn
// der Editor sie speichert). Ohne Datei gilt die eingebaute Tabelle
// (world::standard_terrain_materials()). Renderer und Physik bekommen dieselbe Tabelle.
export module app.terrain_materials;

import ngine.core;
import ngine.asset;
import ngine.filesystem;
import ngine.world;

namespace app {

using namespace ngine;

// Pfad der Standardtabelle im Asset-Ordner.
export constexpr const char* terrain_materials_path = "terrain/standard.terrain-materials";

export class TerrainMaterialLibrary {
public:
    // database darf fehlen und muss sonst die Bibliothek überleben.
    explicit TerrainMaterialLibrary(const asset::AssetDatabase* database) {
        const asset::AssetInfo* info =
            database != nullptr ? database->find_path(terrain_materials_path) : nullptr;
        if (info != nullptr) {
            path_ = filesystem::normalize(info->file);
            watcher_.emplace(filesystem::parent_path(path_));
            (void)watcher_->scan();
        }
        table_ = load();
    }

    [[nodiscard]] const world::TerrainMaterials& table() const noexcept {
        return table_;
    }
    // Datei der Tabelle, leer ohne Asset-Ordner.
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

    // Lädt neu, wenn sich die Datei geändert hat; true, wenn die Tabelle jetzt anders ist. Eine
    // fehlerhafte Datei lässt die alte Tabelle stehen.
    bool poll() {
        if (!watcher_) {
            return false;
        }
        bool changed = false;
        for (const filesystem::FileEvent& event : watcher_->poll()) {
            changed = changed || filesystem::normalize(event.path) == path_;
        }
        if (!changed) {
            return false;
        }
        auto table = world::load_terrain_materials_file(path_);
        if (!table) {
            log::warn("Terrain-Materialien: {}", table.error());
            return false;
        }
        if (*table == table_) {
            return false;
        }
        log::info("Terrain-Materialien neu geladen ({} Materialien)", table->materials.size());
        table_ = *std::move(table);
        return true;
    }

    // Übernimmt eine Tabelle sofort (Editor), ohne sie zu speichern.
    void set(world::TerrainMaterials table) {
        table_ = std::move(table);
    }

private:
    [[nodiscard]] world::TerrainMaterials load() const {
        if (!path_.empty()) {
            auto table = world::load_terrain_materials_file(path_);
            if (table) {
                log::info("Terrain-Materialien aus {} ({} Materialien)", path_,
                          table->materials.size());
                return *std::move(table);
            }
            log::warn("Terrain-Materialien: {}; nehme die eingebaute Tabelle", table.error());
        }
        return world::standard_terrain_materials();
    }

    std::string path_;
    std::optional<filesystem::FileWatcher> watcher_;
    world::TerrainMaterials table_;
};

} // namespace app
