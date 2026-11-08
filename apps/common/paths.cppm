module;

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Ordner der Anwendungen finden (seit R2.5 gemeinsam für Sandbox, Editor und ngine_cook).
export module app.paths;

import ngine.filesystem;

namespace app {

using namespace ngine;

// Sucht relative (z. B. „assets“) ab dem Arbeitsordner und ab dem Ordner der Anwendung aufwärts,
// bis zur Wurzel. marker ist eine Datei, die im gefundenen Ordner liegen muss (leer: keine).
export std::optional<std::string> find_upwards(std::string_view relative,
                                               std::string_view marker = {}) {
    std::vector<std::string> starts{filesystem::normalize(".")};
    if (auto exe = filesystem::executable_dir()) {
        starts.push_back(filesystem::normalize(*exe));
    }
    for (std::string directory : starts) {
        while (true) {
            const std::string candidate = filesystem::join(directory, relative);
            if (filesystem::is_directory(candidate) &&
                (marker.empty() || filesystem::exists(filesystem::join(candidate, marker)))) {
                return filesystem::normalize(candidate);
            }
            const std::string parent = filesystem::parent_path(directory);
            if (parent.empty() || parent == directory) {
                break;
            }
            directory = parent;
        }
    }
    return std::nullopt;
}

} // namespace app
