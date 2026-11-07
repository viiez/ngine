module;

#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.math;

namespace ngine::render {

using math::DVec3;
using math::Vec3;

void DebugDraw::line(DVec3 a, DVec3 b, Vec3 color) {
    if (enabled) {
        lines_.push_back({a, b, color});
    }
}

void DebugDraw::box(const math::Affine& transform, Vec3 local_min, Vec3 local_max, Vec3 color) {
    if (!enabled) {
        return;
    }
    // Ecke i: Bit 0 wählt x, Bit 1 y, Bit 2 z.
    DVec3 corners[8];
    for (u32 i = 0; i < 8; ++i) {
        const Vec3 local{(i & 1) != 0 ? local_max.x : local_min.x,
                         (i & 2) != 0 ? local_max.y : local_min.y,
                         (i & 4) != 0 ? local_max.z : local_min.z};
        corners[i] = transform.transform_point(math::to_f64(local));
    }
    // Kanten verbinden Ecken, die sich in genau einem Bit unterscheiden.
    for (u32 i = 0; i < 8; ++i) {
        for (const u32 bit : {1u, 2u, 4u}) {
            if ((i & bit) == 0) {
                lines_.push_back({corners[i], corners[i | bit], color});
            }
        }
    }
}

void DebugDraw::aabb(DVec3 min, DVec3 max, Vec3 color) {
    const Vec3 half = math::to_f32((max - min) * 0.5);
    const math::Affine transform{.linear = math::mat3_identity(), .translation = (min + max) * 0.5};
    box(transform, -half, half, color);
}

void DebugDraw::sphere(DVec3 center, f32 radius, Vec3 color, u32 segments) {
    if (!enabled || segments < 3) {
        return;
    }
    const f64 r = static_cast<f64>(radius);
    for (u32 axis = 0; axis < 3; ++axis) {
        DVec3 previous{};
        for (u32 i = 0; i <= segments; ++i) {
            const f64 angle = 2.0 * math::pi_f64 * static_cast<f64>(i) / static_cast<f64>(segments);
            const f64 c = std::cos(angle) * r;
            const f64 s = std::sin(angle) * r;
            DVec3 offset{};
            if (axis == 0) {
                offset = {0.0, c, s};
            } else if (axis == 1) {
                offset = {c, 0.0, s};
            } else {
                offset = {c, s, 0.0};
            }
            const DVec3 point = center + offset;
            if (i > 0) {
                lines_.push_back({previous, point, color});
            }
            previous = point;
        }
    }
}

void DebugDraw::axes(const math::Affine& transform, f32 size) {
    if (!enabled) {
        return;
    }
    const DVec3 origin = transform.translation;
    const Vec3 colors[3] = {{1.0f, 0.1f, 0.1f}, {0.1f, 1.0f, 0.1f}, {0.2f, 0.3f, 1.0f}};
    for (u32 axis = 0; axis < 3; ++axis) {
        const Vec3 direction = math::normalize(transform.linear[axis]) * size;
        lines_.push_back({origin, origin + math::to_f64(direction), colors[axis]});
    }
}

void DebugDraw::text(DVec3 position, std::string text, Vec3 color) {
    if (enabled) {
        texts_.push_back({position, std::move(text), color});
    }
}

void DebugDraw::clear() noexcept {
    lines_.clear();
    texts_.clear();
}

std::span<const DebugText> DebugDraw::texts() const noexcept {
    return texts_;
}

std::span<const DebugLine> DebugDraw::lines() const noexcept {
    return lines_;
}

bool clip_line(Vec3& a, Vec3& b, Vec3 forward, f32 near_plane) {
    const f32 da = math::dot(a, forward) - near_plane;
    const f32 db = math::dot(b, forward) - near_plane;
    if (da < 0.0f && db < 0.0f) {
        return false;
    }
    if (da < 0.0f) {
        a = a + (b - a) * (da / (da - db));
    } else if (db < 0.0f) {
        b = b + (a - b) * (db / (db - da));
    }
    return true;
}

std::vector<GpuDebugLine> prepare_lines(std::span<const DebugLine> lines, DVec3 camera,
                                        Vec3 forward, f32 near_plane) {
    std::vector<GpuDebugLine> prepared;
    prepared.reserve(lines.size());
    // Etwas vor der nahen Ebene, damit die Linien nicht von ihr abgeschnitten werden.
    const f32 plane = near_plane * 1.01f;
    for (const DebugLine& line : lines) {
        Vec3 a = math::to_f32(line.a - camera);
        Vec3 b = math::to_f32(line.b - camera);
        if (!clip_line(a, b, forward, plane)) {
            continue;
        }
        prepared.push_back(
            {math::extend(a, 1.0f), math::extend(b, 1.0f), math::extend(line.color, 1.0f)});
    }
    return prepared;
}

} // namespace ngine::render
