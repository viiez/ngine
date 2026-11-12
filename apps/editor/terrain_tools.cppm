module;

#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <string>

// Voxel-Werkzeuge im Viewport des Editors (EDIT-08, R3.4): Werkzeugleiste mit Pinsel, Radius,
// Stärke und Material, Pinselvorschau als Kreis auf dem Terrain unter der Maus und Pinselstriche
// mit gedrückter linker Maustaste. Die Logik (Befehle mit Deltas, ein Strich ein Schritt im
// Verlauf) steckt in edit::TerrainBrushTool; hier wird nur bedient und gezeichnet.
export module editor.terrain_tools;

import ngine.core;
import ngine.edit;
import ngine.editor_ui;
import ngine.math;
import ngine.world;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

struct ModeEntry {
    voxel::BrushMode mode;
    const char* label;
};

constexpr std::array modes{
    ModeEntry{voxel::BrushMode::Remove, "Abtragen"}, ModeEntry{voxel::BrushMode::Add, "Auftragen"},
    ModeEntry{voxel::BrushMode::Smooth, "Glätten"},  ModeEntry{voxel::BrushMode::Flatten, "Ebnen"},
    ModeEntry{voxel::BrushMode::Paint, "Bemalen"},
};

ui::UiColor mode_color(voxel::BrushMode mode) {
    switch (mode) {
    case voxel::BrushMode::Remove: return {0.95f, 0.35f, 0.3f, 1.0f};
    case voxel::BrushMode::Add: return {0.4f, 0.85f, 0.35f, 1.0f};
    case voxel::BrushMode::Smooth: return {0.4f, 0.7f, 1.0f, 1.0f};
    case voxel::BrushMode::Flatten: return {1.0f, 0.85f, 0.3f, 1.0f};
    case voxel::BrushMode::Paint: return {0.85f, 0.5f, 1.0f, 1.0f};
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
}

// Materialien der Standardtabelle (voxel::grass bis voxel::sand).
const std::array<std::string, 4>& material_names() {
    static const std::array<std::string, 4> names{"Gras", "Fels", "Erde", "Sand"};
    return names;
}

export class TerrainTools {
public:
    edit::TerrainBrushTool tool;
    // Pinsel an: Linksklick ins Bild formt das ausgewählte Terrain statt auszuwählen.
    bool active = false;

    // Ausgewähltes Terrain, sonst 0.
    [[nodiscard]] static edit::EntityId selected_terrain(edit::SceneDocument& document,
                                                         const edit::Selection& selection) {
        const u64 entity = document.entity_of(selection.current());
        return entity != 0 && document.world().entity(entity).has<world::Terrain>()
                   ? selection.current()
                   : 0;
    }

    // Zweite Zeile der Werkzeugleiste, nur wenn ein Terrain ausgewählt ist.
    void draw_toolbar(edit::SceneDocument& document, const edit::Selection& selection) {
        if (selected_terrain(document, selection) == 0) {
            active = false;
            return;
        }
        ui::begin_disabled(document.playing());
        ui::checkbox("Pinsel", active);
        edit::TerrainBrushSettings& settings = tool.settings;
        for (const ModeEntry& entry : modes) {
            ui::same_line();
            if (ui::radio_button(entry.label, settings.mode == entry.mode)) {
                settings.mode = entry.mode;
                active = true;
            }
        }
        ui::same_line(24.0f);
        constexpr f64 min_radius = 0.5;
        constexpr f64 max_radius = 32.0;
        ui::set_next_item_width(90.0f);
        ui::drag_scalars("Radius", ui::DataType::F32, &settings.radius, 1, 0.05f, &min_radius,
                         &max_radius, "%.1f m");
        if (settings.mode == voxel::BrushMode::Smooth ||
            settings.mode == voxel::BrushMode::Flatten) {
            constexpr f64 min_strength = 0.0;
            constexpr f64 max_strength = 1.0;
            ui::same_line();
            ui::set_next_item_width(70.0f);
            ui::drag_scalars("Stärke", ui::DataType::F32, &settings.strength, 1, 0.01f,
                             &min_strength, &max_strength, "%.2f");
        }
        if (settings.mode == voxel::BrushMode::Add || settings.mode == voxel::BrushMode::Paint) {
            usize material = settings.material >= voxel::grass && settings.material <= voxel::sand
                                 ? settings.material - voxel::grass
                                 : 1;
            ui::same_line();
            ui::set_next_item_width(80.0f);
            if (ui::combo("Material", material, material_names())) {
                settings.material = static_cast<voxel::Material>(voxel::grass + material);
            }
        }
        ui::end_disabled();
    }

    // Direkt nach dem Bild des Viewports: Vorschau unter der Maus, Pinselstrich mit gedrückter
    // linker Maustaste. Liefert true, wenn der Pinsel die Maus benutzt (dann wählt der Klick nicht
    // aus und der Gizmo bleibt still).
    bool update(edit::SceneDocument& document, const edit::Selection& selection,
                const edit::ViewCamera& camera, ui::Point corner, bool hovered, f32 dt) {
        const edit::EntityId terrain = selected_terrain(document, selection);
        if (!active || terrain == 0 || document.playing()) {
            tool.end_stroke();
            hit_.reset();
            return false;
        }
        const bool down = ui::is_mouse_down(ui::MouseButton::Left);
        if (!down) {
            tool.end_stroke();
        }
        hit_.reset();
        if (hovered || tool.stroking()) {
            const ui::Point mouse = ui::mouse_position();
            const math::Ray ray = camera.ray(mouse.x - corner.x, mouse.y - corner.y);
            const math::Vec3 direction{static_cast<f32>(ray.direction.x),
                                       static_cast<f32>(ray.direction.y),
                                       static_cast<f32>(ray.direction.z)};
            const world::Entity entity = document.world().entity(document.entity_of(terrain));
            hit_ = world::raycast_terrain(entity, ray.origin, direction, max_reach);
        }
        const bool starts = hovered && ui::is_mouse_clicked(ui::MouseButton::Left);
        if (hit_ && down && (starts || tool.stroking())) {
            if (auto result = tool.stroke(document, terrain, *hit_, dt); !result) {
                log::warn("{}", result.error());
                tool.end_stroke();
            }
        }
        // Mit gedrücktem Pinsel gehört die Maus ihm, auch über der Luft.
        return hovered && (starts || tool.stroking() || down);
    }

    // Kreis auf dem Terrain unter der Maus (Pinselvorschau), mit Beschriftung.
    void draw(const edit::ViewCamera& camera, ui::Point corner) const {
        if (!active || !hit_) {
            return;
        }
        const ui::UiColor color = mode_color(tool.settings.mode);
        constexpr u32 segments = 48;
        const f64 radius = tool.settings.radius;
        std::optional<math::Vec2> previous;
        for (u32 i = 0; i <= segments; ++i) {
            const f64 angle = 2.0 * static_cast<f64>(math::pi) * static_cast<f64>(i) / segments;
            const math::DVec3 point =
                *hit_ + math::DVec3{std::cos(angle) * radius, 0.0, std::sin(angle) * radius};
            const auto projected = camera.project(point);
            if (projected && previous) {
                ui::draw_line({corner.x + previous->x, corner.y + previous->y},
                              {corner.x + projected->x, corner.y + projected->y}, color, 2.0f);
            }
            previous = projected;
        }
        if (const auto center = camera.project(*hit_)) {
            ui::draw_circle_filled({corner.x + center->x, corner.y + center->y}, 3.0f, color);
            const std::string label =
                std::format("{} {:.1f} m", edit::brush_label(tool.settings.mode), radius);
            ui::draw_text({corner.x + center->x + 10.0f, corner.y + center->y + 6.0f}, color,
                          label);
        }
    }

private:
    // Wie weit der Pinsel von der Kamera reicht, in Metern.
    static constexpr f64 max_reach = 400.0;
    std::optional<math::DVec3> hit_;
};

} // namespace editor
