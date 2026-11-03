module;

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

module ngine.voxel;

import ngine.core;
import ngine.math;

namespace ngine::voxel {

using math::length;
using math::normalize;

namespace {

// Bereich der Samples, den eine Kugel plus Rand berührt, geklemmt an das Gitter.
Box affected(const Grid& grid, Vec3 center, f32 reach) {
    const Extent3 size = grid.size();
    auto lo = [](f32 v) { return static_cast<u32>(std::max(0.0f, std::floor(v))); };
    auto hi = [](f32 v, u32 n) {
        return static_cast<u32>(std::clamp(std::ceil(v) + 1.0f, 0.0f, static_cast<f32>(n)));
    };
    return {
        std::min(lo(center.x - reach), size.x), std::min(lo(center.y - reach), size.y),
        std::min(lo(center.z - reach), size.z), hi(center.x + reach, size.x),
        hi(center.y + reach, size.y),           hi(center.z + reach, size.z),
    };
}

Vec3 to_vec(u32 x, u32 y, u32 z) {
    return {static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)};
}

void add_or_remove(Grid& grid, const Brush& brush, const Box& box) {
    for (u32 z = box.min_z; z < box.max_z; ++z) {
        for (u32 y = box.min_y; y < box.max_y; ++y) {
            for (u32 x = box.min_x; x < box.max_x; ++x) {
                const f32 sphere = brush.radius - length(to_vec(x, y, z) - brush.center);
                Sample s = grid.sample(x, y, z);
                const f32 old = dequantize(s.density);
                if (brush.mode == BrushMode::Add) {
                    // Vereinigung: Maximum der Abstände (positiv innen).
                    if (sphere > old) {
                        s.density = quantize(sphere);
                        if (s.density > 0 && s.material == air) {
                            s.material = brush.material;
                        }
                    }
                } else {
                    // Differenz: Minimum mit dem negierten Kugelabstand.
                    if (-sphere < old) {
                        s.density = quantize(-sphere);
                        if (s.density <= 0) {
                            s.material = air;
                        }
                    }
                }
                grid.set(x, y, z, s);
            }
        }
    }
}

void smooth(Grid& grid, const Brush& brush, const Box& box) {
    // Erst alle neuen Werte berechnen, dann schreiben, damit die Reihenfolge keine Rolle spielt.
    const Extent3 size = grid.size();
    std::vector<i8> result;
    result.reserve(usize{box.max_x - box.min_x} * (box.max_y - box.min_y) *
                   (box.max_z - box.min_z));
    for (u32 z = box.min_z; z < box.max_z; ++z) {
        for (u32 y = box.min_y; y < box.max_y; ++y) {
            for (u32 x = box.min_x; x < box.max_x; ++x) {
                const f32 dist = length(to_vec(x, y, z) - brush.center);
                const f32 weight =
                    std::clamp(1.0f - dist / brush.radius, 0.0f, 1.0f) * brush.strength;
                f32 sum = 0.0f;
                u32 count = 0;
                for (u32 nz = (z > 0 ? z - 1 : z); nz <= std::min(z + 1, size.z - 1); ++nz) {
                    for (u32 ny = (y > 0 ? y - 1 : y); ny <= std::min(y + 1, size.y - 1); ++ny) {
                        for (u32 nx = (x > 0 ? x - 1 : x); nx <= std::min(x + 1, size.x - 1);
                             ++nx) {
                            sum += grid.distance(nx, ny, nz);
                            ++count;
                        }
                    }
                }
                const f32 old = grid.distance(x, y, z);
                const f32 average = sum / static_cast<f32>(count);
                result.push_back(quantize(old + (average - old) * weight));
            }
        }
    }
    usize i = 0;
    for (u32 z = box.min_z; z < box.max_z; ++z) {
        for (u32 y = box.min_y; y < box.max_y; ++y) {
            for (u32 x = box.min_x; x < box.max_x; ++x) {
                Sample s = grid.sample(x, y, z);
                s.density = result[i++];
                if (s.density <= 0) {
                    s.material = air;
                } else if (s.material == air) {
                    // Neu fest gewordene Samples übernehmen das Material eines festen Nachbarn.
                    s.material = brush.material;
                    for (u32 n = 0; n < 6; ++n) {
                        const i32 dx = n == 0 ? -1 : n == 1 ? 1 : 0;
                        const i32 dy = n == 2 ? -1 : n == 3 ? 1 : 0;
                        const i32 dz = n == 4 ? -1 : n == 5 ? 1 : 0;
                        const i64 sx = i64{x} + dx;
                        const i64 sy = i64{y} + dy;
                        const i64 sz = i64{z} + dz;
                        if (sx < 0 || sy < 0 || sz < 0 || sx >= size.x || sy >= size.y ||
                            sz >= size.z) {
                            continue;
                        }
                        const Material m = grid.sample(static_cast<u32>(sx), static_cast<u32>(sy),
                                                       static_cast<u32>(sz))
                                               .material;
                        if (m != air) {
                            s.material = m;
                            break;
                        }
                    }
                }
                grid.set(x, y, z, s);
            }
        }
    }
}

} // namespace

Box apply(Grid& grid, const Brush& brush) {
    // Außerhalb der Kugel ändert sich nur der geklemmte Abstand bis max_distance.
    const f32 reach = brush.mode == BrushMode::Smooth ? brush.radius : brush.radius + max_distance;
    const Box box = affected(grid, brush.center, reach);
    if (box.empty() || brush.radius <= 0.0f) {
        return {};
    }
    if (brush.mode == BrushMode::Smooth) {
        smooth(grid, brush, box);
    } else {
        add_or_remove(grid, brush, box);
    }
    grid.touch();
    return box;
}

std::optional<RayHit> raycast(const Grid& grid, Vec3 origin, Vec3 direction, f32 max_length) {
    constexpr f32 step = 0.25f;
    const Extent3 size = grid.size();
    auto inside_grid = [&](Vec3 p) {
        return p.x >= 0.0f && p.y >= 0.0f && p.z >= 0.0f && p.x <= static_cast<f32>(size.x - 1) &&
               p.y <= static_cast<f32>(size.y - 1) && p.z <= static_cast<f32>(size.z - 1);
    };

    f32 previous_t = 0.0f;
    f32 previous_d = inside_grid(origin) ? grid.distance_at(origin) : -1.0f;
    for (f32 t = step; t <= max_length; t += step) {
        const Vec3 p = origin + direction * t;
        if (!inside_grid(p)) {
            previous_t = t;
            previous_d = -1.0f;
            continue;
        }
        const f32 d = grid.distance_at(p);
        if (d > 0.0f && previous_d <= 0.0f) {
            // Linear zwischen dem letzten Punkt in der Luft und dem ersten im Stoff.
            const f32 f = previous_d < 0.0f ? previous_d / (previous_d - d) : 0.0f;
            const f32 hit_t = previous_t + (t - previous_t) * f;
            return RayHit{origin + direction * hit_t, hit_t};
        }
        previous_t = t;
        previous_d = d;
    }
    return std::nullopt;
}

} // namespace ngine::voxel
