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

Vec3 to_vec(i64 x, i64 y, i64 z) {
    return {static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)};
}

// Zugriff der Pinsel auf ein Gitter: Koordinaten außerhalb gibt es nicht.
struct GridAccess {
    Grid* grid;

    [[nodiscard]] bool contains(i64 x, i64 y, i64 z) const noexcept {
        const Extent3 size = grid->size();
        return x >= 0 && y >= 0 && z >= 0 && x < size.x && y < size.y && z < size.z;
    }
    [[nodiscard]] Sample get(i64 x, i64 y, i64 z) const noexcept {
        return grid->sample(static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(z));
    }
    bool put(i64 x, i64 y, i64 z, Sample sample) const noexcept {
        grid->set(static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(z), sample);
        return true;
    }
};

// Zugriff auf alle Chunks eines Terrains: unbegrenzt, fehlende Chunks entstehen beim Schreiben.
struct StoreAccess {
    ChunkStore* store;

    [[nodiscard]] static bool contains(i64 /*x*/, i64 /*y*/, i64 /*z*/) noexcept {
        return true;
    }
    [[nodiscard]] Sample get(i64 x, i64 y, i64 z) const noexcept {
        return store->sample(x, y, z);
    }
    bool put(i64 x, i64 y, i64 z, Sample sample) const {
        return store->set(x, y, z, sample);
    }
};

// Bereich der Samples, den eine Kugel plus Rand berührt (ungeklemmt).
SampleBox reach_box(Vec3 center, f32 reach) {
    auto lo = [](f32 v) { return static_cast<i64>(std::floor(v)); };
    auto hi = [](f32 v) { return static_cast<i64>(std::ceil(v)) + 1; };
    return {lo(center.x - reach), lo(center.y - reach), lo(center.z - reach),
            hi(center.x + reach), hi(center.y + reach), hi(center.z + reach)};
}

// Material für ein fest gewordenes Sample: das eines festen Nachbarn, sonst das des Pinsels.
template <typename Access>
Material neighbour_material(const Access& access, const Brush& brush, i64 x, i64 y, i64 z) {
    for (u32 n = 0; n < 6; ++n) {
        const i64 sx = x + (n == 0 ? -1 : n == 1 ? 1 : 0);
        const i64 sy = y + (n == 2 ? -1 : n == 3 ? 1 : 0);
        const i64 sz = z + (n == 4 ? -1 : n == 5 ? 1 : 0);
        if (!access.contains(sx, sy, sz)) {
            continue;
        }
        const Material m = access.get(sx, sy, sz).material;
        if (m != air) {
            return m;
        }
    }
    return brush.material;
}

template <typename Access>
bool add_or_remove(const Access& access, const Brush& brush, const SampleBox& box) {
    bool changed = false;
    for (i64 z = box.min_z; z < box.max_z; ++z) {
        for (i64 y = box.min_y; y < box.max_y; ++y) {
            for (i64 x = box.min_x; x < box.max_x; ++x) {
                const f32 sphere = brush.radius - length(to_vec(x, y, z) - brush.center);
                Sample s = access.get(x, y, z);
                const Sample before = s;
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
                if (s != before) {
                    changed = access.put(x, y, z, s) || changed;
                }
            }
        }
    }
    return changed;
}

template <typename Access>
bool smooth(const Access& access, const Brush& brush, const SampleBox& box) {
    // Erst alle neuen Werte berechnen, dann schreiben, damit die Reihenfolge keine Rolle spielt.
    std::vector<i8> result;
    result.reserve(static_cast<usize>((box.max_x - box.min_x) * (box.max_y - box.min_y) *
                                      (box.max_z - box.min_z)));
    for (i64 z = box.min_z; z < box.max_z; ++z) {
        for (i64 y = box.min_y; y < box.max_y; ++y) {
            for (i64 x = box.min_x; x < box.max_x; ++x) {
                const f32 dist = length(to_vec(x, y, z) - brush.center);
                const f32 weight =
                    std::clamp(1.0f - dist / brush.radius, 0.0f, 1.0f) * brush.strength;
                f32 sum = 0.0f;
                u32 count = 0;
                for (i64 nz = z - 1; nz <= z + 1; ++nz) {
                    for (i64 ny = y - 1; ny <= y + 1; ++ny) {
                        for (i64 nx = x - 1; nx <= x + 1; ++nx) {
                            if (access.contains(nx, ny, nz)) {
                                sum += dequantize(access.get(nx, ny, nz).density);
                                ++count;
                            }
                        }
                    }
                }
                const f32 old = dequantize(access.get(x, y, z).density);
                const f32 average = sum / static_cast<f32>(count);
                result.push_back(quantize(old + (average - old) * weight));
            }
        }
    }
    bool changed = false;
    usize i = 0;
    for (i64 z = box.min_z; z < box.max_z; ++z) {
        for (i64 y = box.min_y; y < box.max_y; ++y) {
            for (i64 x = box.min_x; x < box.max_x; ++x) {
                Sample s = access.get(x, y, z);
                const Sample before = s;
                s.density = result[i++];
                if (s.density <= 0) {
                    s.material = air;
                } else if (s.material == air) {
                    // Neu fest gewordene Samples übernehmen das Material eines festen Nachbarn.
                    s.material = neighbour_material(access, brush, x, y, z);
                }
                if (s != before) {
                    changed = access.put(x, y, z, s) || changed;
                }
            }
        }
    }
    return changed;
}

// Ebnen (R3.4): Abstand zur waagrechten Ebene durch die Mitte, positiv darunter. Zum Rand der Kugel
// hin weich ausgeblendet, damit keine Stufe entsteht.
template <typename Access>
bool flatten(const Access& access, const Brush& brush, const SampleBox& box) {
    bool changed = false;
    for (i64 z = box.min_z; z < box.max_z; ++z) {
        for (i64 y = box.min_y; y < box.max_y; ++y) {
            for (i64 x = box.min_x; x < box.max_x; ++x) {
                const f32 dist = length(to_vec(x, y, z) - brush.center);
                const f32 t = std::clamp(1.0f - dist / brush.radius, 0.0f, 1.0f);
                const f32 weight = std::min(1.0f, t * 2.0f) * brush.strength;
                if (weight <= 0.0f) {
                    continue;
                }
                Sample s = access.get(x, y, z);
                const Sample before = s;
                const f32 old = dequantize(s.density);
                const f32 target =
                    std::clamp(brush.center.y - static_cast<f32>(y), -max_distance, max_distance);
                s.density = quantize(old + (target - old) * weight);
                if (s.density <= 0) {
                    s.material = air;
                } else if (s.material == air) {
                    s.material = neighbour_material(access, brush, x, y, z);
                }
                if (s != before) {
                    changed = access.put(x, y, z, s) || changed;
                }
            }
        }
    }
    return changed;
}

// Bemalen (R3.4): feste Samples in der Kugel bekommen das Material des Pinsels.
template <typename Access>
bool paint(const Access& access, const Brush& brush, const SampleBox& box) {
    bool changed = false;
    for (i64 z = box.min_z; z < box.max_z; ++z) {
        for (i64 y = box.min_y; y < box.max_y; ++y) {
            for (i64 x = box.min_x; x < box.max_x; ++x) {
                if (length(to_vec(x, y, z) - brush.center) > brush.radius) {
                    continue;
                }
                Sample s = access.get(x, y, z);
                if (s.material != air && s.material != brush.material) {
                    s.material = brush.material;
                    changed = access.put(x, y, z, s) || changed;
                }
            }
        }
    }
    return changed;
}

template <typename Access>
bool apply_brush(const Access& access, const Brush& brush, const SampleBox& box) {
    switch (brush.mode) {
    case BrushMode::Smooth: return smooth(access, brush, box);
    case BrushMode::Flatten: return flatten(access, brush, box);
    case BrushMode::Paint: return brush.material != air && paint(access, brush, box);
    case BrushMode::Add:
    case BrushMode::Remove: break;
    }
    return add_or_remove(access, brush, box);
}

// Außerhalb der Kugel ändert sich beim Auftragen und Abtragen nur der geklemmte Abstand bis
// max_distance; die anderen Pinsel wirken nur in der Kugel.
f32 brush_reach(const Brush& brush) {
    return brush.mode == BrushMode::Add || brush.mode == BrushMode::Remove
               ? brush.radius + max_distance
               : brush.radius;
}

// Strahltest über eine Abstandsfunktion; inside sagt, wo es Samples gibt.
template <typename Distance, typename Inside>
std::optional<RayHit> march(const Distance& distance_at, const Inside& inside, Vec3 origin,
                            Vec3 direction, f32 max_length) {
    constexpr f32 step = 0.25f;
    f32 previous_t = 0.0f;
    f32 previous_d = inside(origin) ? distance_at(origin) : -1.0f;
    for (f32 t = step; t <= max_length; t += step) {
        const Vec3 p = origin + direction * t;
        if (!inside(p)) {
            previous_t = t;
            previous_d = -1.0f;
            continue;
        }
        const f32 d = distance_at(p);
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

} // namespace

SampleBox brush_box(const Brush& brush) {
    return reach_box(brush.center, brush_reach(brush));
}

Box apply(Grid& grid, const Brush& brush) {
    if (brush.radius <= 0.0f) {
        return {};
    }
    const Extent3 size = grid.size();
    SampleBox box = reach_box(brush.center, brush_reach(brush));
    box.min_x = std::clamp<i64>(box.min_x, 0, size.x);
    box.min_y = std::clamp<i64>(box.min_y, 0, size.y);
    box.min_z = std::clamp<i64>(box.min_z, 0, size.z);
    box.max_x = std::clamp<i64>(box.max_x, 0, size.x);
    box.max_y = std::clamp<i64>(box.max_y, 0, size.y);
    box.max_z = std::clamp<i64>(box.max_z, 0, size.z);
    if (box.empty()) {
        return {};
    }
    apply_brush(GridAccess{&grid}, brush, box);
    grid.touch();
    return {static_cast<u32>(box.min_x), static_cast<u32>(box.min_y), static_cast<u32>(box.min_z),
            static_cast<u32>(box.max_x), static_cast<u32>(box.max_y), static_cast<u32>(box.max_z)};
}

SampleBox apply(ChunkStore& store, const Brush& brush) {
    if (brush.radius <= 0.0f) {
        return {};
    }
    const SampleBox box = reach_box(brush.center, brush_reach(brush));
    if (!apply_brush(StoreAccess{&store}, brush, box)) {
        return {};
    }
    // Berührte Chunks wieder auf einen Wert schrumpfen, wenn sie ganz Luft oder ganz fest sind.
    const ChunkCoord low = chunk_of(box.min_x, box.min_y, box.min_z);
    const ChunkCoord high = chunk_of(box.max_x - 1, box.max_y - 1, box.max_z - 1);
    for (i32 z = low.z; z <= high.z; ++z) {
        for (i32 y = low.y; y <= high.y; ++y) {
            for (i32 x = low.x; x <= high.x; ++x) {
                if (Chunk* chunk = store.find({x, y, z, 0})) {
                    chunk->compact();
                }
            }
        }
    }
    store.mark_changed(box);
    return box;
}

std::optional<RayHit> raycast(const Grid& grid, Vec3 origin, Vec3 direction, f32 max_length) {
    const Extent3 size = grid.size();
    auto inside_grid = [&](Vec3 p) {
        return p.x >= 0.0f && p.y >= 0.0f && p.z >= 0.0f && p.x <= static_cast<f32>(size.x - 1) &&
               p.y <= static_cast<f32>(size.y - 1) && p.z <= static_cast<f32>(size.z - 1);
    };
    return march([&](Vec3 p) { return grid.distance_at(p); }, inside_grid, origin, direction,
                 max_length);
}

std::optional<RayHit> raycast(const ChunkStore& store, Vec3 origin, Vec3 direction,
                              f32 max_length) {
    return march([&](Vec3 p) { return store.distance_at(p); }, [](Vec3) { return true; }, origin,
                 direction, max_length);
}

} // namespace ngine::voxel
