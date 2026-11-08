module;

#include <format>
#include <string>
#include <string_view>
#include <utility>

// Prefabs im Editor (R2.5, WORLD-04): Auswahl als Prefab speichern (Vorlage in assets/prefabs und
// die Auswahl wird ihre erste Instanz), Instanz aufbrechen, Änderungen einer Instanz in die
// Vorlage übernehmen und Prefabs aus dem Asset-Browser in den Viewport ziehen. Die Vorlage
// schreibt der Editor als Datei; Hot Reload lädt sie neu, und die Prefab-Bibliothek der Szene
// zieht alle Instanzen nach (Überschreibungen bleiben).
export module editor.prefab_tools;

import ngine.core;
import ngine.asset;
import ngine.edit;
import ngine.filesystem;
import ngine.math;
import ngine.world;
import app.prefabs;

namespace editor {

using namespace ngine;

// Ordner der Vorlagen im Asset-Ordner.
constexpr std::string_view prefab_dir = "prefabs";

// Erlaubte Zeichen eines Dateinamens aus dem Namen der Entity; alles andere wird „_“.
std::string file_stem(std::string_view name) {
    std::string stem;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_';
        stem.push_back(plain ? c : '_');
    }
    return stem.empty() ? std::string{"prefab"} : stem;
}

export class PrefabTools {
public:
    // Speichert den Teilbaum unter selected als neue Vorlage und macht ihn zu ihrer Instanz (ein
    // Undo-Schritt löst die Verknüpfung, die Datei bleibt).
    static void save_as_prefab(edit::SceneDocument& document, edit::EntityId selected,
                               asset::AssetDatabase& database) {
        world::World& world = document.world();
        const u64 entity = document.entity_of(selected);
        if (entity == 0 || document.read_only()) {
            return;
        }
        if (world::instance_root(world, entity) != 0) {
            log::error("Die Entity gehört schon zu einer Prefab-Instanz; erst aufbrechen");
            return;
        }
        const auto* name = world.entity(entity).get<world::Name>();
        const std::string stem = file_stem(name != nullptr ? name->value : std::string_view{});
        // Freien Dateinamen suchen: laterne.prefab, laterne_2.prefab, …
        std::string relative;
        for (u32 n = 1;; ++n) {
            relative = n == 1 ? std::format("{}/{}.prefab", prefab_dir, stem)
                              : std::format("{}/{}_{}.prefab", prefab_dir, stem, n);
            if (!filesystem::exists(filesystem::join(database.root(), relative))) {
                break;
            }
        }
        const std::string file = filesystem::join(database.root(), relative);
        if (auto written = filesystem::write_text(
                file, world::write_prefab(world::make_prefab(world, entity)));
            !written) {
            log::error("Prefab: {}", written.error());
            return;
        }
        if (auto changes = database.refresh(); !changes) {
            log::error("Assets: {}", changes.error());
            return;
        }
        const asset::AssetInfo* info = database.find_path(relative);
        if (info == nullptr) {
            log::error("Prefab {} fehlt nach dem Einlesen in der Datenbank", relative);
            return;
        }
        auto command = edit::link_prefab(document, selected, info->id);
        if (!command) {
            log::error("{}", command.error());
            return;
        }
        if (auto result = document.execute(*std::move(command)); !result) {
            log::error("{}", result.error());
            return;
        }
        document.history().seal();
        log::info("Prefab gespeichert: {}", relative);
    }

    // Bricht die Instanz auf, zu der selected gehört.
    static void unpack(edit::SceneDocument& document, edit::EntityId selected) {
        const u64 root = world::instance_root(document.world(), document.entity_of(selected));
        if (root == 0 || document.read_only()) {
            return;
        }
        auto command = edit::unpack_prefab(document, document.id_of(root));
        if (!command) {
            log::error("{}", command.error());
            return;
        }
        if (auto result = document.execute(*std::move(command)); !result) {
            log::error("{}", result.error());
            return;
        }
        document.history().seal();
    }

    // Schreibt die Instanz, zu der selected gehört, als neuen Stand ihrer Vorlage. Hot Reload lädt
    // die Datei neu und zieht die anderen Instanzen nach.
    static void apply_to_prefab(edit::SceneDocument& document, edit::EntityId selected,
                                const asset::AssetDatabase& database) {
        world::World& world = document.world();
        const u64 root = world::instance_root(world, document.entity_of(selected));
        if (root == 0) {
            return;
        }
        const u64 prefab_id = world.entity(root).get<world::PrefabInstance>()->prefab;
        const asset::AssetInfo* info = database.find(prefab_id);
        if (info == nullptr) {
            log::error("Die Vorlage {} gibt es im Asset-Ordner nicht", prefab_id);
            return;
        }
        if (auto written = filesystem::write_text(
                info->file, world::write_prefab(world::make_prefab(world, root)));
            !written) {
            log::error("Prefab: {}", written.error());
            return;
        }
        log::info("Prefab übernommen: {}", info->path);
    }

    // Instanz einer Vorlage aus dem Asset-Browser an position einfügen (ein Undo-Schritt) und
    // auswählen.
    static void insert(edit::SceneDocument& document, edit::Selection& selection,
                       app::PrefabLibrary& library, const asset::AssetInfo& info,
                       math::DVec3 position) {
        if (document.read_only()) {
            return;
        }
        const world::Prefab* prefab = library.get(info.id);
        if (prefab == nullptr) {
            log::error("Prefab {} lässt sich nicht laden", info.path);
            return;
        }
        auto snapshot = edit::prefab_snapshot(document, *prefab, info.id, position);
        if (!snapshot) {
            log::error("{}", snapshot.error());
            return;
        }
        const edit::EntityId root = snapshot->nodes.front().id;
        if (auto result = document.execute(edit::insert_entities(
                *std::move(snapshot),
                std::format("'{}' einfügen", filesystem::file_name(info.path))));
            !result) {
            log::error("{}", result.error());
            return;
        }
        document.history().seal();
        selection.select(root);
    }
};

} // namespace editor
