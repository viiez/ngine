module;

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.math;
import ngine.world;

namespace ngine::render {

namespace {

using math::Vec3;
using math::Vec4;

// Richtung der lokalen -z-Achse in der Welt.
Vec3 forward(const world::WorldTransform& transform) {
    return -math::normalize(transform.value.linear[2]);
}

f32 light_type(LightType type) {
    return static_cast<f32>(static_cast<u32>(type));
}

} // namespace

LightList collect_lights(world::World& world, math::DVec3 origin) {
    LightList list;
    bool warned = false;
    const auto push = [&](const GpuLight& light) {
        if (list.lights.size() >= max_lights) {
            if (!warned) {
                log::warn("Mehr als {} Lichter; die übrigen bleiben dunkel", max_lights);
                warned = true;
            }
            return;
        }
        list.lights.push_back(light);
    };

    world.each<const world::WorldTransform, const DirectionalLight>(
        [&](const world::WorldTransform& transform, const DirectionalLight& light) {
            if (!light.active) {
                return;
            }
            const Vec3 direction = forward(transform);
            const Vec3 radiance = light.color * light.intensity;
            if (!list.sun) {
                list.sun = Sun{
                    .to_sun = -direction, .radiance = radiance, .cast_shadows = light.cast_shadows};
                return;
            }
            push({.position_range = {0.0f, 0.0f, 0.0f, 0.0f},
                  .color_type = math::extend(radiance, light_type(LightType::Directional)),
                  .direction = math::extend(direction, 0.0f),
                  .spot = {}});
        });
    world.each<const world::WorldTransform, const PointLight>(
        [&](const world::WorldTransform& transform, const PointLight& light) {
            if (!light.active) {
                return;
            }
            push({.position_range = math::extend(math::to_f32(transform.position() - origin),
                                                 std::max(light.range, 0.0f)),
                  .color_type =
                      math::extend(light.color * light.intensity, light_type(LightType::Point)),
                  .direction = {0.0f, -1.0f, 0.0f, 0.0f},
                  .spot = {}});
        });
    world.each<const world::WorldTransform, const SpotLight>(
        [&](const world::WorldTransform& transform, const SpotLight& light) {
            if (!light.active) {
                return;
            }
            const f32 outer = std::clamp(light.outer_angle, 0.0f, math::pi * 0.5f);
            const f32 inner = std::clamp(light.inner_angle, 0.0f, outer);
            const f32 cos_outer = std::cos(outer);
            const f32 scale = 1.0f / std::max(0.001f, std::cos(inner) - cos_outer);
            push({.position_range = math::extend(math::to_f32(transform.position() - origin),
                                                 std::max(light.range, 0.0f)),
                  .color_type =
                      math::extend(light.color * light.intensity, light_type(LightType::Spot)),
                  .direction = math::extend(forward(transform), 0.0f),
                  .spot = {scale, -cos_outer * scale, 0.0f, 0.0f}});
        });
    return list;
}

} // namespace ngine::render
