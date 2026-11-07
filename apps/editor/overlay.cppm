module;

#include <algorithm>
#include <cmath>
#include <string>

// Hilfslinien im Viewport des Editors: Raster am Boden, Lichter als Symbole und die Auswahl. Szenen
// aus Dateien haben bis R2.4 keine Meshes (MeshRenderer ist eine Laufzeitkomponente), deshalb
// braucht der Editor diese Linien, um überhaupt etwas zu zeigen. Seit R2.3 dazu Namen als Text in
// der Welt (REND-15): an Lichtern und an der Auswahl.
export module editor.overlay;

import ngine.core;
import ngine.edit;
import ngine.math;
import ngine.render;
import ngine.world;

namespace editor {

using namespace ngine;
using math::DVec3;
using math::Vec3;

constexpr Vec3 grid_color{0.32f, 0.32f, 0.34f};
constexpr Vec3 selection_color{1.0f, 0.55f, 0.1f};
constexpr Vec3 label_color{0.8f, 0.8f, 0.82f};

void draw_grid(render::DebugDraw& debug) {
    constexpr i32 half = 20;
    for (i32 i = -half; i <= half; ++i) {
        const f64 offset = static_cast<f64>(i);
        const Vec3 color = i == 0 ? Vec3{0.45f, 0.45f, 0.5f} : grid_color;
        debug.line(DVec3{offset, 0.0, -half}, DVec3{offset, 0.0, half}, color);
        debug.line(DVec3{-half, 0.0, offset}, DVec3{half, 0.0, offset}, color);
    }
}

Vec3 forward_of(const world::WorldTransform& transform) {
    return -math::normalize(transform.value.linear[2]);
}

export void draw_overlay(render::DebugDraw& debug, render::MeshStore& meshes,
                         edit::SceneDocument& document, const edit::Selection& selection) {
    world::World& world = document.world();
    world::update_transforms(world);
    draw_grid(debug);
    const u64 selected = document.entity_of(selection.current());
    const auto label = [&](world::Entity entity, DVec3 position) {
        if (entity.id() != selected) {
            debug.text(position, std::string{entity.name()}, label_color);
        }
    };

    world.each<const world::WorldTransform, const render::PointLight>(
        [&](world::Entity entity, const world::WorldTransform& transform,
            const render::PointLight& light) {
            debug.sphere(transform.position(), 0.25f, light.color, 12);
            label(entity, transform.position());
        });
    world.each<const world::WorldTransform, const render::SpotLight>(
        [&](world::Entity entity, const world::WorldTransform& transform,
            const render::SpotLight& light) {
            debug.sphere(transform.position(), 0.25f, light.color, 12);
            label(entity, transform.position());
            const f32 length = light.range > 0.0f ? std::min(light.range, 5.0f) : 5.0f;
            debug.line(transform.position(),
                       transform.position() + math::to_f64(forward_of(transform) * length),
                       light.color);
        });
    world.each<const world::WorldTransform, const render::DirectionalLight>(
        [&](world::Entity entity, const world::WorldTransform& transform,
            const render::DirectionalLight& light) {
            // Die Sonne hat keinen Ort: Pfeil über dem Ursprung in Richtung des Lichts.
            const DVec3 start{0.0, 6.0, 0.0};
            debug.line(start, start + math::to_f64(forward_of(transform) * 3.0f), light.color);
            debug.sphere(start, 0.3f, light.color, 12);
            label(entity, start);
        });

    if (selected == 0) {
        return;
    }
    const world::Entity entity = world.entity(selected);
    const auto* transform = entity.get<world::WorldTransform>();
    if (transform == nullptr) {
        return;
    }
    const auto* renderer = entity.get<render::MeshRenderer>();
    if (const render::GpuMesh* mesh = renderer != nullptr ? meshes.get(renderer->mesh) : nullptr) {
        debug.box(transform->value, mesh->bounds_min, mesh->bounds_max, selection_color);
    } else {
        debug.box(transform->value, Vec3{-0.3f, -0.3f, -0.3f}, Vec3{0.3f, 0.3f, 0.3f},
                  selection_color);
    }
    // Name der Auswahl, auch ohne Name die Kennung wie im Szenenbaum.
    debug.text(transform->position(), edit::display_name(world, selected, selection.current()),
               selection_color);
}

} // namespace editor
