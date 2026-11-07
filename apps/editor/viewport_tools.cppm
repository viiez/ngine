module;

#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Werkzeuge im Viewport des Editors (R2.3): Werkzeugleiste (Gizmo-Modus, Achsen, Raster, Spielen),
// Bedienung der Gizmos mit der Maus und Zeichnen von Gizmos und Text in der Welt über das Bild.
// Die Logik der Gizmos steckt in edit::Gizmo; hier wird nur projiziert und gezeichnet, über die
// Zeichenliste der UI, damit Gizmos immer über der Szene liegen (ADR 0031).
export module editor.viewport_tools;

import ngine.core;
import ngine.edit;
import ngine.editor_ui;
import ngine.math;
import ngine.render;
import editor.menu;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

constexpr ui::UiColor highlight{1.0f, 0.85f, 0.2f, 1.0f};

ui::UiColor axis_color(edit::GizmoAxis axis) {
    switch (axis) {
    case edit::GizmoAxis::X: return {0.92f, 0.28f, 0.28f, 1.0f};
    case edit::GizmoAxis::Y: return {0.38f, 0.82f, 0.3f, 1.0f};
    case edit::GizmoAxis::Z: return {0.3f, 0.52f, 0.96f, 1.0f};
    default: return {0.85f, 0.85f, 0.85f, 1.0f};
    }
}

ui::Point add(ui::Point a, math::Vec2 b) {
    return {a.x + b.x, a.y + b.y};
}

// Text mit dunklem Schatten, damit er auf hellem und dunklem Grund lesbar bleibt.
void shadowed_text(ui::Point position, ui::UiColor color, std::string_view text) {
    ui::draw_text({position.x + 1.0f, position.y + 1.0f}, {0.0f, 0.0f, 0.0f, 0.8f}, text);
    ui::draw_text(position, color, text);
}

// Kamera des Viewports für Treffertests und Projektion.
export edit::ViewCamera view_camera(const render::CameraView& view, ui::Extent extent) {
    return edit::ViewCamera{.position = view.position,
                            .rotation = view.rotation,
                            .fov_y = view.camera.fov_y,
                            .near_plane = view.camera.near_plane,
                            .width = static_cast<f32>(extent.width),
                            .height = static_cast<f32>(extent.height)};
}

export class ViewportTools {
public:
    edit::Gizmo gizmo;

    // Werkzeugleiste über dem Bild.
    void draw_toolbar(edit::SceneDocument& document, PlayControls& play) {
        const bool playing = document.playing();
        ui::begin_disabled(playing);
        if (ui::radio_button("Verschieben (W)", gizmo.mode == edit::GizmoMode::Translate)) {
            gizmo.mode = edit::GizmoMode::Translate;
        }
        ui::same_line();
        if (ui::radio_button("Drehen (E)", gizmo.mode == edit::GizmoMode::Rotate)) {
            gizmo.mode = edit::GizmoMode::Rotate;
        }
        ui::same_line();
        if (ui::radio_button("Skalieren (R)", gizmo.mode == edit::GizmoMode::Scale)) {
            gizmo.mode = edit::GizmoMode::Scale;
        }
        ui::same_line(24.0f);
        bool local = gizmo.space == edit::GizmoSpace::Local;
        if (ui::checkbox("Lokal", local)) {
            gizmo.space = local ? edit::GizmoSpace::Local : edit::GizmoSpace::World;
        }
        ui::same_line();
        ui::checkbox("Raster", gizmo.snap.enabled);
        ui::end_disabled();
        ui::same_line(24.0f);
        if (ui::button(playing ? "Stopp (F5)" : "Spielen (F5)")) {
            play.toggle(document, gizmo);
        }
        if (playing) {
            ui::same_line();
            ui::checkbox("Pause", play.paused);
            ui::same_line();
            ui::text_colored({1.0f, 0.55f, 0.2f, 1.0f}, "Spielt");
        }
    }

    // Kurzbefehle W, E, R; nicht in Eingabefeldern und nicht mit Strg.
    void handle_keys() {
        if (ui::is_any_item_active() || ui::is_key_down(ui::Key::Ctrl)) {
            return;
        }
        if (ui::is_key_pressed(ui::Key::W)) {
            gizmo.mode = edit::GizmoMode::Translate;
        } else if (ui::is_key_pressed(ui::Key::E)) {
            gizmo.mode = edit::GizmoMode::Rotate;
        } else if (ui::is_key_pressed(ui::Key::R)) {
            gizmo.mode = edit::GizmoMode::Scale;
        }
    }

    // Direkt nach dem Bild des Viewports: Gizmo mit der Maus bedienen. corner ist die linke obere
    // Ecke des Bildes. Liefert true, wenn der Gizmo die Maus benutzt hat.
    bool update(edit::SceneDocument& document, const edit::Selection& selection,
                const edit::ViewCamera& camera, ui::Point corner, bool hovered) {
        const edit::EntityId entity = selection.current();
        if (entity == 0 || document.playing()) {
            gizmo.reset();
            return false;
        }
        const ui::Point mouse = ui::mouse_position();
        const edit::GizmoInput input{.x = mouse.x - corner.x,
                                     .y = mouse.y - corner.y,
                                     .hovered = hovered,
                                     .pressed = ui::is_mouse_clicked(ui::MouseButton::Left),
                                     .down = ui::is_mouse_down(ui::MouseButton::Left)};
        // Strg kehrt das Raster beim Ziehen um.
        const bool snap = gizmo.snap.enabled;
        gizmo.snap.enabled = snap != ui::is_key_down(ui::Key::Ctrl);
        const bool used = gizmo.update(document, entity, camera, input);
        gizmo.snap.enabled = snap;
        return used;
    }

    // Gizmo der Auswahl und Texte in der Welt über das Bild zeichnen.
    void draw(edit::SceneDocument& document, const edit::Selection& selection,
              const edit::ViewCamera& camera, ui::Point corner,
              std::span<const render::DebugText> texts) const {
        for (const render::DebugText& text : texts) {
            if (const auto p = camera.project(text.position)) {
                shadowed_text(add(corner, *p + math::Vec2{8.0f, -8.0f}),
                              {text.color.x, text.color.y, text.color.z, 1.0f}, text.text);
            }
        }
        if (selection.current() == 0 || document.playing()) {
            return;
        }
        const auto frame = gizmo.frame(document, selection.current(), camera);
        if (!frame) {
            return;
        }
        const edit::GizmoAxis focus = gizmo.dragging() ? gizmo.active() : gizmo.hovered();
        const auto color_of = [&](edit::GizmoAxis axis) {
            return axis == focus ? highlight : axis_color(axis);
        };
        const edit::GizmoShape shape = gizmo.shape(*frame);
        for (const edit::GizmoShape::Line& line : shape.lines) {
            // Gezogen wird nur eine Achse: die anderen blass.
            if (gizmo.dragging() && line.axis != focus) {
                continue;
            }
            const auto a = camera.project(line.a);
            const auto b = camera.project(line.b);
            if (a && b) {
                ui::draw_line(add(corner, *a), add(corner, *b), color_of(line.axis),
                              line.axis == focus ? 4.0f : 3.0f);
            }
        }
        for (const edit::GizmoShape::Marker& marker : shape.markers) {
            if (gizmo.dragging() && marker.axis != focus) {
                continue;
            }
            draw_marker(camera, corner, *frame, marker, color_of(marker.axis));
        }
        if (gizmo.dragging()) {
            const ui::Point mouse = ui::mouse_position();
            shadowed_text({mouse.x + 18.0f, mouse.y + 10.0f}, highlight, gizmo.drag_label());
        }
    }

private:
    static void draw_marker(const edit::ViewCamera& camera, ui::Point corner,
                            const edit::GizmoFrame& frame, const edit::GizmoShape::Marker& marker,
                            ui::UiColor color) {
        const auto center = camera.project(marker.position);
        if (!center) {
            return;
        }
        if (marker.handle == edit::GizmoShape::Handle::Box) {
            constexpr f32 half = 5.0f;
            const ui::Point p = add(corner, *center);
            ui::draw_rect_filled({p.x - half, p.y - half}, {p.x + half, p.y + half}, color);
            return;
        }
        // Pfeilspitze: Dreieck entlang der projizierten Achse.
        const auto tip = camera.project(marker.position + marker.direction * (frame.size * 0.18));
        if (!tip) {
            return;
        }
        math::Vec2 along = *tip - *center;
        const f32 length = math::length(along);
        if (length < 1.0f) {
            return;
        }
        along = along / length;
        const math::Vec2 side{-along.y * 6.0f, along.x * 6.0f};
        ui::draw_triangle_filled(add(corner, *tip), add(corner, *center + side),
                                 add(corner, *center - side), color);
    }
};

} // namespace editor
