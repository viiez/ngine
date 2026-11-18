#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// ngine_bindgen (R4.4): schreibt die C#-Structs aller Komponenten der Engine aus der Reflection
// nach <Datei>. Die Datei wird nur geschrieben, wenn sich etwas geändert hat, damit C# nicht bei
// jedem Bauen neu übersetzt.

import ngine.core;
import ngine.filesystem;
import ngine.reflect;
import ngine.world;
import ngine.physics;
import ngine.audio;
import ngine.animation;
import ngine.render;
import ngine.script;

using namespace ngine;

int main(int argc, char** argv) {
    if (argc != 2) {
        log::error("Aufruf: ngine_bindgen <Components.g.cs>");
        return 2;
    }
    const std::string path = argv[1];
    reflect::TypeRegistry types;
    world::register_types(types);
    physics::register_types(types);
    audio::register_types(types);
    animation::register_types(types);
    render::register_types(types);
    const std::string source = script::generate_bindings(types);

    if (const auto existing = filesystem::read_text(path); existing && *existing == source) {
        return 0;
    }
    if (auto created = filesystem::create_directories(filesystem::parent_path(path)); !created) {
        log::error("Ordner für {} lässt sich nicht anlegen: {}", path, created.error());
        return 1;
    }
    const auto bytes = std::as_bytes(std::span{source});
    if (auto written = filesystem::write_file(path, bytes); !written) {
        log::error("{} lässt sich nicht schreiben: {}", path, written.error());
        return 1;
    }
    log::info("C#-Bindings geschrieben: {}", path);
    return 0;
}
