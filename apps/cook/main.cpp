// ngine_cook (R2.5, ASSET-02): kocht alle Modelle eines Asset-Ordners in dessen Cache
// (<assets>/.cache): glTF mit MikkTSpace-Tangenten in das gekochte Format, Texturen als KTX2 mit
// Basis Universal. Aktuelle Einträge bleiben, außer mit --force.
//
// Optionen:
//   --assets <pfad>  Asset-Ordner (sonst „assets“ ab Arbeits- und Programmordner aufwärts)
//   --cache <pfad>   Cache-Ordner (sonst <assets>/.cache)
//   --force          auch aktuelle Modelle neu kochen
//   --etc1s          Texturen als ETC1S (klein, verlustbehaftet) statt UASTC
//   --raw            Texturen ohne Basis Universal (RGBA8 in KTX2, nur zum Vergleich)
//
// Rückgabe 0, wenn alles gekocht ist; 1, wenn etwas fehlschlug.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

import ngine.core;
import ngine.asset;
import ngine.cook;
import app.paths;

namespace {

using namespace ngine;

struct Options {
    std::optional<std::string> assets;
    std::optional<std::string> cache;
    bool force = false;
    cook::CookSettings settings;
};

Options parse_options(std::span<char*> args) {
    Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "--assets" && i + 1 < args.size()) {
            options.assets = std::string{args[++i]};
        } else if (arg == "--cache" && i + 1 < args.size()) {
            options.cache = std::string{args[++i]};
        } else if (arg == "--force") {
            options.force = true;
        } else if (arg == "--etc1s") {
            options.settings.textures.uastc = false;
        } else if (arg == "--raw") {
            options.settings.textures.basis = false;
        } else {
            log::warn("Unbekannte Option: {}", arg);
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(std::span{argv, static_cast<std::size_t>(argc)});
    const auto root = options.assets ? options.assets : app::find_upwards("assets");
    if (!root) {
        log::fatal("Kein Asset-Ordner gefunden (Option --assets <ordner>)");
        return 1;
    }
    auto database = asset::AssetDatabase::open(*root);
    if (!database) {
        log::fatal("{}", database.error());
        return 1;
    }
    const asset::AssetCache cache{options.cache ? *options.cache
                                                : asset::default_cache_dir(database->root())};
    log::info("Koche {} nach {}", database->root(), cache.directory());
    const cook::CookReport report =
        cook::cook_all(*database, cache, options.settings, options.force);
    for (const std::string& error : report.errors) {
        log::error("{}", error);
    }
    log::info("{} gekocht, {} schon aktuell, {} fehlgeschlagen in {:.2f} s", report.cooked,
              report.current, report.failed, report.seconds);
    log::flush();
    return report.failed == 0 ? 0 : 1;
}
