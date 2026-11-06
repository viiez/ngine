module;

#include <memory>
#include <string>
#include <utility>
#include <vector>

// Szenenbaum des Editors (EDIT-03): alle Entities mit ihrer Hierarchie. Klick wählt aus, F2 oder
// das Kontextmenü benennt um, Ziehen auf eine andere Entity hängt um, Ziehen auf die freie Fläche
// macht zur Wurzel, Entf löscht. Alle Änderungen laufen als Befehle über das Dokument.
export module editor.scene_tree;

import ngine.core;
import ngine.edit;
import ngine.editor_ui;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

constexpr std::string_view payload_type = "NGINE_ENTITY";

void run(edit::SceneDocument& document, std::unique_ptr<edit::Command> command) {
    if (auto result = document.execute(std::move(command)); !result) {
        log::error("{}", result.error());
    }
}

export class SceneTreePanel {
public:
    // Neue Entity unter parent (0: Wurzel), gleich ausgewählt und zum Umbenennen offen.
    void create(edit::SceneDocument& document, edit::Selection& selection, edit::EntityId parent) {
        auto created = edit::create_entity(document, "Entity", parent);
        if (auto result = document.execute(std::move(created.command)); !result) {
            log::error("{}", result.error());
            return;
        }
        selection.select(created.id);
        start_rename(document, created.id);
    }

    void start_rename(edit::SceneDocument& document, edit::EntityId id) {
        const u64 entity = document.entity_of(id);
        if (entity == 0) {
            return;
        }
        renaming_ = id;
        rename_buffer_ = edit::display_name(document.world(), entity, id);
        focus_rename_ = true;
    }

    void draw(edit::SceneDocument& document, edit::Selection& selection) {
        if (renaming_ != 0 && !document.exists(renaming_)) {
            renaming_ = 0;
        }
        if (ui::button("Neue Entity")) {
            create(document, selection, 0);
        }
        ui::separator();

        if (ui::begin_child("Baum")) {
            draw_rows(document, selection);
            // Freie Fläche unter den Zeilen: Ablegen macht zur Wurzel, Rechtsklick legt an.
            const ui::Size rest = ui::content_region_available();
            ui::dummy({rest.width, rest.height > 20.0f ? rest.height : 20.0f});
            if (const auto dropped = ui::drop_target(payload_type)) {
                reparent(document, *dropped, 0);
            }
            if (ui::is_item_clicked()) {
                selection.clear();
            }
            if (ui::begin_popup_context_window("Baum-Kontext")) {
                if (ui::menu_item("Neue Entity")) {
                    create(document, selection, 0);
                }
                ui::end_popup();
            }
            if (ui::is_window_focused() && renaming_ == 0 && !ui::is_any_item_active()) {
                if (ui::is_key_pressed(ui::Key::F2) && selection.current() != 0) {
                    start_rename(document, selection.current());
                }
                if (ui::is_key_pressed(ui::Key::Delete) && selection.current() != 0) {
                    run(document, edit::destroy_entity(selection.current()));
                }
            }
        }
        ui::end_child();
    }

private:
    void reparent(edit::SceneDocument& document, edit::EntityId child, edit::EntityId parent) {
        if (!edit::can_reparent(document, child, parent)) {
            log::warn("{} lässt sich dort nicht einhängen", child);
            return;
        }
        run(document, edit::reparent(child, parent));
    }

    void draw_rows(edit::SceneDocument& document, edit::Selection& selection) {
        const std::vector<edit::TreeRow> rows = edit::build_tree(document);
        // Tiefen der offenen Knoten; Kinder eines geschlossenen Knotens werden übersprungen.
        std::vector<u32> open;
        i64 closed_depth = -1;
        for (const edit::TreeRow& row : rows) {
            if (closed_depth >= 0 && row.depth > static_cast<u64>(closed_depth)) {
                continue;
            }
            closed_depth = -1;
            while (!open.empty() && open.back() >= row.depth) {
                ui::tree_pop();
                open.pop_back();
            }

            ui::TreeFlags flags = ui::TreeFlags::DefaultOpen;
            if (!row.has_children) {
                flags = flags | ui::TreeFlags::Leaf;
            }
            if (selection.selected(row.id)) {
                flags = flags | ui::TreeFlags::Selected;
            }
            const bool renaming = renaming_ == row.id;
            const bool is_open =
                ui::tree_node(row.id, renaming ? std::string_view{} : row.name, flags);
            if (!renaming) {
                if (ui::is_item_clicked()) {
                    selection.select(row.id);
                    scrolled_to_ = row.id;
                } else if (selection.selected(row.id) && scrolled_to_ != row.id) {
                    // Anderswo ausgewählt (z. B. im Viewport): Zeile in den sichtbaren Bereich.
                    ui::scroll_here(0.5f);
                    scrolled_to_ = row.id;
                }
                ui::drag_source(payload_type, row.id, row.name);
                if (const auto dropped = ui::drop_target(payload_type)) {
                    reparent(document, *dropped, row.id);
                }
                ui::push_id(row.id);
                if (ui::begin_popup_context_item("Kontext")) {
                    selection.select(row.id);
                    draw_context_menu(document, selection, row.id);
                    ui::end_popup();
                }
                ui::pop_id();
            } else {
                ui::same_line();
                draw_rename_field(document);
            }

            if (row.has_children) {
                if (is_open) {
                    open.push_back(row.depth);
                } else {
                    closed_depth = row.depth;
                }
            }
        }
        while (!open.empty()) {
            ui::tree_pop();
            open.pop_back();
        }
    }

    void draw_context_menu(edit::SceneDocument& document, edit::Selection& selection,
                           edit::EntityId id) {
        if (ui::menu_item("Kind anlegen")) {
            create(document, selection, id);
        }
        if (ui::menu_item("Umbenennen", "F2")) {
            start_rename(document, id);
        }
        if (ui::menu_item("Zur Wurzel machen")) {
            reparent(document, id, 0);
        }
        ui::separator();
        if (ui::menu_item("Löschen", "Entf")) {
            run(document, edit::destroy_entity(id));
        }
    }

    void draw_rename_field(edit::SceneDocument& document) {
        ui::set_next_item_width(-1.0f);
        if (focus_rename_) {
            ui::set_keyboard_focus_here();
            focus_rename_ = false;
        }
        ui::push_id(renaming_);
        const bool entered = ui::input_text("##Name", rename_buffer_, true);
        const bool left = ui::is_item_deactivated();
        ui::pop_id();
        if (entered || (left && !ui::is_key_pressed(ui::Key::Escape))) {
            const u64 entity = document.entity_of(renaming_);
            if (!rename_buffer_.empty() && entity != 0 &&
                rename_buffer_ != edit::display_name(document.world(), entity, renaming_)) {
                run(document, edit::rename(renaming_, rename_buffer_));
            }
            renaming_ = 0;
        } else if (left) {
            renaming_ = 0;
        }
    }

    // Zuletzt in den sichtbaren Bereich gescrollte Auswahl.
    edit::EntityId scrolled_to_ = 0;
    edit::EntityId renaming_ = 0;
    std::string rename_buffer_;
    bool focus_rename_ = false;
};

} // namespace editor
