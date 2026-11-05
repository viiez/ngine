module;

#include <algorithm>
#include <array>
#include <cmath>

module ngine.render;

import ngine.core;
import ngine.math;

namespace ngine::render {

using math::DVec3;
using math::Vec3;
using math::Vec4;

std::array<f32, cascade_count> cascade_splits(f32 near_plane, f32 distance, f32 lambda) {
    const f32 n = std::max(near_plane, 1e-4f);
    const f32 d = std::max(distance, n * 1.01f);
    const f32 l = std::clamp(lambda, 0.0f, 1.0f);
    std::array<f32, cascade_count> splits{};
    for (u32 i = 0; i < cascade_count; ++i) {
        const f32 t = static_cast<f32>(i + 1) / static_cast<f32>(cascade_count);
        const f32 logarithmic = n * std::pow(d / n, t);
        const f32 uniform = n + (d - n) * t;
        splits[i] = l * logarithmic + (1.0f - l) * uniform;
    }
    splits[cascade_count - 1] = d;
    return splits;
}

bool Cascade::may_cast(Vec3 sphere_center, f32 sphere_radius) const noexcept {
    const Vec3 d = sphere_center - center;
    const f32 x = math::dot(d, axis_x);
    const f32 y = math::dot(d, axis_y);
    const f32 z = math::dot(d, axis_z);
    const f32 reach = radius + sphere_radius;
    return std::abs(x) <= reach && std::abs(y) <= reach && z >= -reach &&
           z <= reach + caster_distance;
}

std::array<Cascade, cascade_count> fit_cascades(const CascadeView& view, Vec3 to_sun,
                                                const ShadowSettings& settings) {
    const std::array<f32, cascade_count> splits =
        cascade_splits(view.near_plane, settings.distance, settings.split_lambda);
    const f32 tan_y = std::tan(view.fov_y * 0.5f);
    const f32 tan_x = tan_y * view.aspect;
    // Abstand der Ecken des Stumpfs von der Blickachse pro Meter Tiefe, zum Quadrat.
    const f32 k2 = tan_x * tan_x + tan_y * tan_y;
    const Vec3 forward = -math::normalize(view.rotation[2]);

    // Lichtraum: z zur Sonne, x und y senkrecht dazu, unabhängig von der Kamera.
    const Vec3 axis_z = math::normalize(to_sun);
    const Vec3 reference =
        std::abs(axis_z.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 axis_x = math::normalize(math::cross(reference, axis_z));
    const Vec3 axis_y = math::cross(axis_z, axis_x);
    const u32 resolution = std::max(settings.resolution, 1u);
    const f32 blend = std::clamp(settings.blend, 0.0f, 0.9f);
    const f32 back = std::max(settings.caster_distance, 0.0f);

    std::array<Cascade, cascade_count> cascades{};
    f32 previous_far = view.near_plane;
    f32 start = view.near_plane;
    for (u32 i = 0; i < cascade_count; ++i) {
        const f32 a = start;
        const f32 b = splits[i];
        // Kleinste Kugel um die acht Ecken des Ausschnitts von a bis b: Mittelpunkt auf der
        // Blickachse, höchstens an der fernen Ebene.
        const f32 z = std::min((a + b) * 0.5f * (1.0f + k2), b);
        f32 radius = std::max(std::sqrt(a * a * k2 + (z - a) * (z - a)),
                              std::sqrt(b * b * k2 + (b - z) * (b - z)));
        // Auf 1/16 m aufrunden, damit Rundungsfehler den Radius nicht von Frame zu Frame ändern.
        radius = std::ceil(radius * 16.0f) / 16.0f;
        const f32 texel = 2.0f * radius / static_cast<f32>(resolution);

        // Mittelpunkt in 64 Bit auf ganze Texel des Lichtraums einrasten.
        const DVec3 center_world = view.position + math::to_f64(forward * z);
        const DVec3 dx = math::to_f64(axis_x);
        const DVec3 dy = math::to_f64(axis_y);
        const f64 cx = math::dot(center_world, dx);
        const f64 cy = math::dot(center_world, dy);
        const f64 t = static_cast<f64>(texel);
        const f64 sx = std::round(cx / t) * t;
        const f64 sy = std::round(cy / t) * t;
        const DVec3 snapped = center_world + dx * (sx - cx) + dy * (sy - cy);
        const Vec3 c = math::to_f32(snapped - view.position);

        // Zeilen der Abbildung: x und y auf [-1, 1], Tiefe von der Sonne (0) bis hinter die
        // Kaskade (1).
        const f32 z_max = radius + back;
        const f32 range = z_max + radius;
        const Vec4 row0 = math::extend(axis_x / radius, -math::dot(c, axis_x) / radius);
        const Vec4 row1 = math::extend(axis_y / radius, -math::dot(c, axis_y) / radius);
        const Vec4 row2 = math::extend(-axis_z / range, (z_max + math::dot(c, axis_z)) / range);
        const Vec4 row3{0.0f, 0.0f, 0.0f, 1.0f};
        const math::Mat4 rows = math::Mat4::from_columns(row0, row1, row2, row3);

        Cascade& cascade = cascades[i];
        cascade.view_projection = math::transpose(rows);
        cascade.split_far = b;
        cascade.blend_start = b - blend * (b - previous_far);
        cascade.texel_size = texel;
        cascade.center = c;
        cascade.axis_x = axis_x;
        cascade.axis_y = axis_y;
        cascade.axis_z = axis_z;
        cascade.radius = radius;
        cascade.caster_distance = back;

        // Die nächste Kaskade beginnt schon am Übergang, damit sie ihn abdeckt.
        start = cascade.blend_start;
        previous_far = b;
    }
    return cascades;
}

} // namespace ngine::render
