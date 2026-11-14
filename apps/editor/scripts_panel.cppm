module;

#include <format>
#include <string>

// Fenster „Skripte“ des Editors (R4.4): Zustand des Baus, Meldungen des Compilers mit Datei und
// Zeile und die geladenen Skript-Klassen. „Neu bauen“ startet dotnet build von Hand; sonst baut
// der Editor, sobald sich eine .cs-Datei in assets/skripte ändert.
export module editor.scripts;

import ngine.core;
import ngine.editor_ui;
import ngine.filesystem;
import ngine.script;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

export constexpr const char* scripts_window = "Skripte";

export class ScriptsPanel {
public:
    void draw(script::ScriptSystem* scripts, bool& open) {
        if (!open) {
            return;
        }
        if (ui::begin_window(scripts_window, &open)) {
            if (scripts == nullptr) {
                ui::text_wrapped("Keine Skripte: .NET fehlt oder assets/skripte/Skripte.csproj "
                                 "gibt es nicht (siehe Konsole).");
            } else {
                draw_status(*scripts);
            }
        }
        ui::end_window();
    }

private:
    static void draw_status(script::ScriptSystem& scripts) {
        const script::ScriptStatus& status = scripts.status();
        if (status.building) {
            ui::text("Baut ...");
        } else if (status.last_build) {
            const script::BuildResult& build = *status.last_build;
            if (build.ok) {
                ui::text_colored({.r = 0.4f, .g = 0.9f, .b = 0.4f},
                                 std::format("Gebaut in {:.1f} s", build.seconds));
            } else if (build.sdk_missing) {
                ui::text_colored({.r = 1.0f, .g = 0.8f, .b = 0.3f},
                                 ".NET-SDK fehlt, es läuft die zuletzt gebaute Assembly");
            } else {
                ui::text_colored({.r = 1.0f, .g = 0.4f, .b = 0.4f},
                                 "Bau gescheitert, die bisherigen Skripte laufen weiter");
            }
        }
        ui::same_line();
        ui::begin_disabled(status.building);
        if (ui::button("Neu bauen")) {
            scripts.rebuild();
        }
        ui::end_disabled();

        if (status.last_build && !status.last_build->diagnostics.empty()) {
            ui::separator();
            for (const script::Diagnostic& d : status.last_build->diagnostics) {
                const std::string line =
                    std::format("{}({},{}): {} {}", filesystem::file_name(d.file), d.line, d.column,
                                d.code, d.message);
                if (d.error) {
                    ui::text_colored({.r = 1.0f, .g = 0.4f, .b = 0.4f}, line);
                } else {
                    ui::text_colored({.r = 1.0f, .g = 0.8f, .b = 0.3f}, line);
                }
                if (ui::is_item_hovered()) {
                    ui::set_tooltip(d.file);
                }
            }
        } else if (status.last_build && !status.last_build->ok &&
                   !status.last_build->output.empty()) {
            ui::separator();
            ui::text_wrapped(status.last_build->output);
        }

        ui::separator();
        if (status.assembly.empty()) {
            ui::text_disabled("Keine Skripte geladen");
            return;
        }
        ui::text(std::format("{} Klasse(n) aus {} (geladen: {}.)", status.classes.size(),
                             filesystem::file_name(status.assembly), status.generation));
        for (const std::string& name : status.classes) {
            ui::text(std::format("  {}", name));
        }
        ui::text_disabled(scripts.active() ? "Laufen (Spielmodus)" : "Laufen beim Spielen");
    }
};

} // namespace editor
