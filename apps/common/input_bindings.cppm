module;

#include <optional>
#include <string>
#include <vector>

// Belegung der Eingabe für Sandbox und Editor (R4.1): eingabe/standard.input aus dem Asset-Ordner,
// im laufenden Betrieb neu geladen, wenn sich die Datei ändert. Ohne Datei gilt die eingebaute
// Belegung (input::ActionMap::standard()).
export module app.input_bindings;

import ngine.core;
import ngine.filesystem;
import ngine.input;
import app.paths;

namespace app {

using namespace ngine;

export class InputBindings {
public:
    // assets_root: Asset-Ordner, sonst wird er gesucht wie von Sandbox und Editor.
    explicit InputBindings(std::optional<std::string> assets_root = std::nullopt) {
        if (!assets_root) {
            assets_root = find_upwards("assets");
        }
        if (assets_root) {
            directory_ = filesystem::join(*assets_root, "eingabe");
            path_ = filesystem::normalize(filesystem::join(directory_, "standard.input"));
            watcher_.emplace(directory_);
            (void)watcher_->scan();
        }
    }

    // Die aktuelle Belegung: aus der Datei, sonst die eingebaute.
    [[nodiscard]] input::ActionMap load() const {
        if (!path_.empty() && filesystem::exists(path_)) {
            auto map = input::ActionMap::load_file(path_);
            if (map) {
                log::info("Eingabe: Belegung aus {} ({} Aktionen)", path_, map->actions().size());
                return *std::move(map);
            }
            log::warn("Eingabe: {}; nehme die eingebaute Belegung", map.error());
        }
        return input::ActionMap::standard();
    }

    // Lädt die Belegung neu, wenn sich die Datei geändert hat. Eine fehlerhafte Datei lässt die
    // alte Belegung stehen.
    void poll(input::ActionState& state) {
        if (!watcher_) {
            return;
        }
        bool changed = false;
        for (const filesystem::FileEvent& event : watcher_->poll()) {
            changed = changed || event.path == path_;
        }
        if (!changed) {
            return;
        }
        auto map = input::ActionMap::load_file(path_);
        if (!map) {
            log::warn("Eingabe: {}", map.error());
            return;
        }
        log::info("Eingabe: Belegung neu geladen ({} Aktionen)", map->actions().size());
        state.set_map(*std::move(map));
    }

private:
    std::string directory_;
    std::string path_;
    std::optional<filesystem::FileWatcher> watcher_;
};

} // namespace app
