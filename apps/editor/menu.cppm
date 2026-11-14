module;

#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

// Menüleiste, Dialoge und Kurzbefehle des Editors (seit R2.3 aus main.cpp ausgelagert): Datei
// (Neu, Öffnen, Speichern, Speichern unter – als .nscene binär –, Beenden) mit Dialog für den Pfad
// und Rückfrage bei ungespeicherten Änderungen, Bearbeiten mit Undo und Redo, Werkzeug (Gizmos),
// Spiel (Spielen, Pause, Stopp) und Ansicht (Fenster, Layout). Die Hauptschleife ruft draw() einmal
// pro Frame.
export module editor.menu;

import ngine.core;
import ngine.edit;
import ngine.editor_ui;
import editor.scene_tree;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

// Namen der Fenster, zugleich ihre Kennung für das Andocken.
export constexpr std::string_view viewport_window = "Viewport";
export constexpr std::string_view console_window = "Konsole";
export constexpr std::string_view stats_window = "Statistik";
export constexpr std::string_view tree_window = "Szene";
export constexpr std::string_view inspector_window = "Inspector";
export constexpr std::string_view assets_window = "Assets";

// Sichtbarkeit der Fenster (Menü „Ansicht“).
export struct Panels {
    bool viewport = true;
    bool tree = true;
    bool inspector = true;
    bool console = true;
    bool stats = true;
    bool assets = true;
    // Materialtabelle des Terrains (R4.3), anfangs zu.
    bool terrain_materials = false;
};

export ui::DockLayout default_layout() {
    return {.center = {std::string{viewport_window}},
            .left = {std::string{tree_window}},
            // Das zuletzt angedockte Fenster liegt vorn.
            .right = {std::string{stats_window}, std::string{inspector_window}},
            // Konsole vorn, Assets dahinter als Reiter.
            .bottom = {std::string{assets_window}, std::string{console_window}}};
}

// Dateiname für Titel und Meldungen.
export std::string file_label(const edit::SceneDocument& document) {
    if (document.path().empty()) {
        return "Unbenannt";
    }
    const std::string& path = document.path();
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Spielen im Editor (EDIT-06): Start sichert die Welt im Dokument, Stopp stellt sie wieder her.
// Die Systeme laufen in der Hauptschleife, solange playing() und nicht paused.
export class PlayControls {
public:
    bool paused = false;
    // Einzelschritt angefordert (R4.1): Die Hauptschleife rechnet genau einen festen Schritt und
    // bleibt in der Pause.
    bool step_requested = false;

    void request_step() noexcept {
        paused = true;
        step_requested = true;
    }

    [[nodiscard]] static bool playing(const edit::SceneDocument& document) noexcept {
        return document.playing();
    }
    void start(edit::SceneDocument& document, edit::Gizmo& gizmo) {
        gizmo.reset();
        if (auto result = document.begin_play(); !result) {
            log::error("{}", result.error());
            return;
        }
        paused = false;
        log::info("Spielen");
    }
    void stop(edit::SceneDocument& document) {
        if (auto result = document.end_play(); !result) {
            log::error("{}", result.error());
            return;
        }
        paused = false;
        log::info("Gestoppt, Welt wiederhergestellt");
    }
    void toggle(edit::SceneDocument& document, edit::Gizmo& gizmo) {
        if (document.playing()) {
            stop(document);
        } else {
            start(document, gizmo);
        }
    }
};

// Teile des Editors, die das Menü bedient.
export struct MenuTargets {
    edit::SceneDocument& document;
    edit::Selection& selection;
    SceneTreePanel& tree;
    Panels& panels;
    edit::Gizmo& gizmo;
    PlayControls& play;
    // Beendet die Hauptschleife.
    std::function<void()> quit;
    // Weitere Menüs hinter „Spiel“ (z. B. Prefab und Assets, R2.5); leer: keine.
    std::function<void()> extra_menus;
    // Fügt ein Voxel-Terrain ein (R3.1); leer: kein Menüeintrag.
    std::function<void()> new_terrain;
    // Pinsel für Terrains (R3.4): Schalter und ob ein Terrain ausgewählt ist; nullptr: kein
    // Eintrag.
    bool* terrain_brush = nullptr;
    bool terrain_selected = false;
};

export class EditorMenu {
public:
    explicit EditorMenu(std::string path) : path_input_{std::move(path)} {}

    // Menüleiste, Dialoge und Kurzbefehle eines Frames; vor den Fenstern aufrufen, die
    // Kurzbefehle wirken erst nach den Fenstern (siehe handle_shortcuts()).
    void draw_menu_bar(MenuTargets& targets) {
        if (!ui::begin_main_menu_bar()) {
            return;
        }
        edit::SceneDocument& document = targets.document;
        if (ui::begin_menu("Datei")) {
            const bool editable = !document.playing();
            if (ui::menu_item("Neu", "Strg+N", nullptr, editable)) {
                request(targets, Pending::New);
            }
            if (ui::menu_item("Öffnen …", "Strg+O", nullptr, editable)) {
                request(targets, Pending::Open);
            }
            if (ui::menu_item("Speichern", "Strg+S", nullptr, editable)) {
                save(document);
            }
            if (ui::menu_item("Speichern unter …", {}, nullptr, editable)) {
                open_path_dialog(PathDialog::SaveAs);
            }
            ui::separator();
            if (ui::menu_item("Beenden", "Alt+F4")) {
                request(targets, Pending::Quit);
            }
            ui::end_menu();
        }
        if (ui::begin_menu("Bearbeiten")) {
            const edit::CommandStack& history = document.history();
            const bool editable = !document.read_only();
            const std::string undo_label = history.can_undo()
                                               ? "Rückgängig: " + history.undo_label()
                                               : std::string{"Rückgängig"};
            const std::string redo_label = history.can_redo()
                                               ? "Wiederholen: " + history.redo_label()
                                               : std::string{"Wiederholen"};
            if (ui::menu_item(undo_label, "Strg+Z", nullptr, editable && history.can_undo())) {
                undo(document);
            }
            if (ui::menu_item(redo_label, "Strg+Y", nullptr, editable && history.can_redo())) {
                redo(document);
            }
            ui::separator();
            if (ui::menu_item("Neue Entity", {}, nullptr, editable)) {
                targets.tree.create(document, targets.selection, 0);
            }
            if (targets.new_terrain && ui::menu_item("Neues Terrain", {}, nullptr, editable)) {
                targets.new_terrain();
            }
            const bool selected = targets.selection.current() != 0;
            if (ui::menu_item("Umbenennen", "F2", nullptr, editable && selected)) {
                targets.tree.start_rename(document, targets.selection.current());
            }
            if (ui::menu_item("Löschen", "Entf", nullptr, editable && selected)) {
                delete_selected(targets);
            }
            ui::end_menu();
        }
        if (ui::begin_menu("Werkzeug")) {
            edit::Gizmo& gizmo = targets.gizmo;
            const auto mode_item = [&](std::string_view label, std::string_view key,
                                       edit::GizmoMode mode) {
                bool active = gizmo.mode == mode;
                if (ui::menu_item(label, key, &active)) {
                    gizmo.mode = mode;
                }
            };
            mode_item("Verschieben", "W", edit::GizmoMode::Translate);
            mode_item("Drehen", "E", edit::GizmoMode::Rotate);
            mode_item("Skalieren", "R", edit::GizmoMode::Scale);
            ui::separator();
            bool local = gizmo.space == edit::GizmoSpace::Local;
            if (ui::menu_item("Lokale Achsen", {}, &local)) {
                gizmo.space = local ? edit::GizmoSpace::Local : edit::GizmoSpace::World;
            }
            ui::menu_item("Raster", "Strg beim Ziehen", &gizmo.snap.enabled);
            if (targets.terrain_brush != nullptr) {
                ui::separator();
                // Ohne ausgewähltes Terrain sagt der Eintrag, was fehlt.
                ui::menu_item(targets.terrain_selected ? "Terrain-Pinsel"
                                                       : "Terrain-Pinsel (erst Terrain auswählen)",
                              {}, targets.terrain_brush,
                              targets.terrain_selected && !document.playing());
            }
            ui::end_menu();
        }
        if (ui::begin_menu("Spiel")) {
            const bool playing = document.playing();
            if (ui::menu_item(playing ? "Stoppen" : "Spielen", "F5")) {
                targets.play.toggle(document, targets.gizmo);
            }
            ui::menu_item("Pause", {}, &targets.play.paused, playing);
            if (ui::menu_item("Einzelschritt", {}, nullptr, playing)) {
                targets.play.request_step();
            }
            ui::end_menu();
        }
        if (targets.extra_menus) {
            targets.extra_menus();
        }
        if (ui::begin_menu("Ansicht")) {
            Panels& panels = targets.panels;
            ui::menu_item(viewport_window, {}, &panels.viewport);
            ui::menu_item(tree_window, {}, &panels.tree);
            ui::menu_item(inspector_window, {}, &panels.inspector);
            ui::menu_item(console_window, {}, &panels.console);
            ui::menu_item(stats_window, {}, &panels.stats);
            ui::menu_item(assets_window, {}, &panels.assets);
            ui::menu_item("Terrain-Materialien", {}, &panels.terrain_materials);
            ui::separator();
            if (ui::menu_item("Layout zurücksetzen")) {
                panels = {};
                ui::reset_layout();
            }
            ui::end_menu();
        }
        ui::end_main_menu_bar();
    }

    // Dialoge für Pfad und ungespeicherte Änderungen.
    void draw_dialogs(MenuTargets& targets) {
        edit::SceneDocument& document = targets.document;
        if (open_path_dialog_) {
            ui::open_popup(path_dialog_ == PathDialog::Open ? open_title : save_title);
            open_path_dialog_ = false;
            if (path_dialog_ == PathDialog::SaveAs && !document.path().empty()) {
                path_input_ = document.path();
            }
        }
        for (const PathDialog kind : {PathDialog::Open, PathDialog::SaveAs}) {
            const std::string_view name = kind == PathDialog::Open ? open_title : save_title;
            if (!ui::begin_popup_modal(name)) {
                continue;
            }
            ui::text("Pfad der Szene (.json als Text, .nscene binär):");
            ui::set_next_item_width(420.0f);
            const bool entered = ui::input_text("##Pfad", path_input_, true);
            const bool confirmed = ui::button("OK") || entered;
            ui::same_line();
            if (ui::button("Abbrechen") || ui::is_key_pressed(ui::Key::Escape)) {
                path_dialog_ = PathDialog::None;
                ui::close_current_popup();
            } else if (confirmed && !path_input_.empty()) {
                if (kind == PathDialog::Open) {
                    open(document, path_input_);
                } else {
                    save_to(document, path_input_);
                }
                path_dialog_ = PathDialog::None;
                ui::close_current_popup();
            }
            ui::end_popup();
        }

        if (open_confirm_) {
            ui::open_popup(confirm_title);
            open_confirm_ = false;
        }
        if (ui::begin_popup_modal(confirm_title)) {
            ui::text(std::format("{} hat ungespeicherte Änderungen.", file_label(document)));
            if (ui::button("Verwerfen")) {
                const Pending action = pending_;
                pending_ = Pending::None;
                ui::close_current_popup();
                run(targets, action);
            }
            ui::same_line();
            if (ui::button("Abbrechen") || ui::is_key_pressed(ui::Key::Escape)) {
                pending_ = Pending::None;
                ui::close_current_popup();
            }
            ui::end_popup();
        }
    }

    // Kurzbefehle; nicht, solange ein Eingabefeld aktiv ist (dort gehören Strg+Z usw. dem Feld).
    void handle_shortcuts(MenuTargets& targets) {
        if (ui::is_any_item_active()) {
            return;
        }
        edit::SceneDocument& document = targets.document;
        if (ui::is_key_pressed(ui::Key::F5)) {
            targets.play.toggle(document, targets.gizmo);
            return;
        }
        if (!ui::is_key_down(ui::Key::Ctrl)) {
            return;
        }
        const bool shift = ui::is_key_down(ui::Key::Shift);
        if (ui::is_key_pressed(ui::Key::Z, true)) {
            shift ? redo(document) : undo(document);
        } else if (ui::is_key_pressed(ui::Key::Y, true)) {
            redo(document);
        } else if (document.playing()) {
            return;
        } else if (ui::is_key_pressed(ui::Key::S)) {
            save(document);
        } else if (ui::is_key_pressed(ui::Key::O)) {
            request(targets, Pending::Open);
        } else if (ui::is_key_pressed(ui::Key::N)) {
            request(targets, Pending::New);
        }
    }

    // Beenden über das Fenster (Schließfeld, Alt+F4): fragt bei ungespeicherten Änderungen nach.
    void request_quit(MenuTargets& targets) {
        request(targets, Pending::Quit);
    }

    void open(edit::SceneDocument& document, const std::string& path) {
        if (auto opened = document.open(path); opened) {
            log::info("Szene geöffnet: {} ({} Entities)", path, *opened);
        } else {
            log::error("{}", opened.error());
        }
    }
    void save_to(edit::SceneDocument& document, const std::string& path) {
        if (auto result = document.save(path); result) {
            log::info("Szene gespeichert: {}", path);
        } else {
            log::error("{}", result.error());
        }
    }

    static void delete_selected(MenuTargets& targets) {
        const edit::EntityId selected = targets.selection.current();
        if (selected == 0 || targets.document.read_only()) {
            return;
        }
        if (auto result = targets.document.execute(edit::destroy_entity(selected)); !result) {
            log::error("{}", result.error());
        }
    }

private:
    // Aktion, die bei ungespeicherten Änderungen erst nachfragt.
    enum class Pending : u8 { None, New, Open, Quit };
    enum class PathDialog : u8 { None, Open, SaveAs };

    static constexpr std::string_view open_title = "Szene öffnen";
    static constexpr std::string_view save_title = "Szene speichern unter";
    static constexpr std::string_view confirm_title = "Ungespeicherte Änderungen";

    void open_path_dialog(PathDialog kind) {
        path_dialog_ = kind;
        open_path_dialog_ = true;
    }

    void save(edit::SceneDocument& document) {
        if (document.path().empty()) {
            open_path_dialog(PathDialog::SaveAs);
        } else {
            save_to(document, document.path());
        }
    }

    static void undo(edit::SceneDocument& document) {
        if (!document.undo()) {
            log::debug("Nichts rückgängig zu machen");
        }
    }
    static void redo(edit::SceneDocument& document) {
        if (!document.redo()) {
            log::debug("Nichts zu wiederholen");
        }
    }

    void run(MenuTargets& targets, Pending action) {
        switch (action) {
        case Pending::New: targets.document.clear(); break;
        case Pending::Open: open_path_dialog(PathDialog::Open); break;
        case Pending::Quit: targets.quit(); break;
        case Pending::None: break;
        }
    }

    // Verwirft ungespeicherte Änderungen erst nach Rückfrage. Ein laufendes Spiel endet vorher.
    void request(MenuTargets& targets, Pending action) {
        if (targets.document.playing()) {
            targets.play.stop(targets.document);
        }
        if (targets.document.dirty()) {
            pending_ = action;
            open_confirm_ = true;
        } else {
            run(targets, action);
        }
    }

    std::string path_input_;
    PathDialog path_dialog_ = PathDialog::None;
    bool open_path_dialog_ = false;
    Pending pending_ = Pending::None;
    bool open_confirm_ = false;
};

} // namespace editor
