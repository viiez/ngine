module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

module ngine.voxel;

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.profile;

namespace ngine::voxel {

namespace {

// Verdoppelte Koordinate eines Punkts knapp außerhalb (d = -1, 1) oder in der Mitte (d = 0) des
// Bereichs [a, a + e) entlang einer Achse; immer ungerade, also nie auf einer Grenze.
constexpr i64 probe2(i64 a, i64 e, i32 d) noexcept {
    if (d < 0) {
        return 2 * a - 1;
    }
    if (d > 0) {
        return 2 * (a + e) + 1;
    }
    return 2 * a + e + 1;
}

std::array<ChunkCoord, 8> children(ChunkCoord c) noexcept {
    std::array<ChunkCoord, 8> result{};
    for (u32 i = 0; i < 8; ++i) {
        result[i] = {c.x * 2 + static_cast<i32>(i & 1u), c.y * 2 + static_cast<i32>((i >> 1) & 1u),
                     c.z * 2 + static_cast<i32>((i >> 2) & 1u), static_cast<u8>(c.level - 1)};
    }
    return result;
}

} // namespace

u8 root_level(const LodDesc& desc) noexcept {
    if (desc.max_level > 0) {
        return std::min(desc.max_level, max_lod_level);
    }
    // Kleinste Stufe, deren Chunks sich im ganzen Sichtbereich noch teilen.
    u8 level = 0;
    while (level < max_lod_level &&
           desc.detail * static_cast<f64>(chunk_extent(level)) < desc.view_distance) {
        ++level;
    }
    return level;
}

LodLayout::LodLayout(std::vector<ChunkCoord> leaves) : leaves_{std::move(leaves)} {
    std::sort(leaves_.begin(), leaves_.end());
    set_.reserve(leaves_.size());
    for (const ChunkCoord c : leaves_) {
        set_.insert(c);
        max_level_ = std::max(max_level_, c.level);
        level_mask_ |= 1u << c.level;
    }
}

std::optional<ChunkCoord> LodLayout::leaf_at2(i64 x2, i64 y2, i64 z2) const noexcept {
    for (u8 level = 0; level <= max_level_; ++level) {
        if ((level_mask_ & (1u << level)) == 0) {
            continue;
        }
        const i64 e2 = 2 * chunk_extent(level);
        const ChunkCoord c{static_cast<i32>(floor_div(x2, e2)), static_cast<i32>(floor_div(y2, e2)),
                           static_cast<i32>(floor_div(z2, e2)), level};
        if (set_.contains(c)) {
            return c;
        }
    }
    return std::nullopt;
}

std::vector<ChunkCoord> LodLayout::neighbourhood(ChunkCoord coord) const {
    // Raster aus halben Chunks (so breit wie ein Blatt eine Stufe feiner) um den Chunk: 4 pro
    // Achse, der Chunk selbst belegt die mittleren 2 × 2 × 2.
    const SampleBox box = chunk_box(coord);
    const i64 half = chunk_extent(coord.level) / 2;
    std::vector<ChunkCoord> result;
    for (i64 z = -1; z <= 2; ++z) {
        for (i64 y = -1; y <= 2; ++y) {
            for (i64 x = -1; x <= 2; ++x) {
                const auto leaf = leaf_at2(2 * (box.min_x + x * half) + half + 1,
                                           2 * (box.min_y + y * half) + half + 1,
                                           2 * (box.min_z + z * half) + half + 1);
                if (leaf && std::find(result.begin(), result.end(), *leaf) == result.end()) {
                    result.push_back(*leaf);
                }
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool LodLayout::balanced() const {
    for (const ChunkCoord leaf : leaves_) {
        const SampleBox b = chunk_box(leaf);
        const i64 e = chunk_extent(leaf.level);
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0 && dz == 0) {
                        continue;
                    }
                    const auto other = leaf_at2(probe2(b.min_x, e, dx), probe2(b.min_y, e, dy),
                                                probe2(b.min_z, e, dz));
                    if (other && other->level > leaf.level + 1) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

std::array<u32, max_lod_level + 1> LodLayout::level_counts() const noexcept {
    std::array<u32, max_lod_level + 1> counts{};
    for (const ChunkCoord c : leaves_) {
        ++counts[c.level];
    }
    return counts;
}

f64 observer_distance(std::span<const math::DVec3> observers, ChunkCoord coord) noexcept {
    const SampleBox b = chunk_box(coord);
    f64 best = 1e300;
    for (const math::DVec3& o : observers) {
        const auto axis = [](f64 v, i64 min, i64 max) {
            const f64 below = static_cast<f64>(min) - v;
            const f64 above = v - static_cast<f64>(max);
            return std::max({below, above, 0.0});
        };
        const f64 dx = axis(o.x, b.min_x, b.max_x);
        const f64 dy = axis(o.y, b.min_y, b.max_y);
        const f64 dz = axis(o.z, b.min_z, b.max_z);
        best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return best;
}

LodLayout select_lod(std::span<const math::DVec3> observers, const LodDesc& desc) {
    NGINE_PROFILE_ZONE("voxel::select_lod");
    if (observers.empty()) {
        return {};
    }
    const u8 top = root_level(desc);
    const i64 y_min = i64{desc.min_chunk_y} * chunk_voxels;
    const i64 y_max = i64{desc.max_chunk_y} * chunk_voxels;
    const auto in_height = [&](ChunkCoord c) {
        const SampleBox b = chunk_box(c);
        return b.min_y < y_max && b.max_y > y_min;
    };

    // Wurzeln um jeden Beobachter.
    const i64 e = chunk_extent(top);
    std::unordered_set<ChunkCoord, ChunkCoordHash> roots;
    for (const math::DVec3& o : observers) {
        const auto first = [&](f64 v) {
            return floor_div(static_cast<i64>(std::floor(v - desc.view_distance)), e);
        };
        const auto last = [&](f64 v) {
            return floor_div(static_cast<i64>(std::ceil(v + desc.view_distance)), e);
        };
        for (i64 z = first(o.z); z <= last(o.z); ++z) {
            for (i64 y = floor_div(y_min, e); y <= floor_div(y_max - 1, e); ++y) {
                for (i64 x = first(o.x); x <= last(o.x); ++x) {
                    roots.insert(
                        {static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z), top});
                }
            }
        }
    }

    // Teilen, solange ein Beobachter nah genug ist.
    std::unordered_set<ChunkCoord, ChunkCoordHash> leaves;
    std::vector<ChunkCoord> stack{roots.begin(), roots.end()};
    while (!stack.empty()) {
        const ChunkCoord c = stack.back();
        stack.pop_back();
        if (!in_height(c)) {
            continue;
        }
        const f64 d = observer_distance(observers, c);
        if (d > desc.view_distance) {
            continue;
        }
        if (c.level > 0 && d < desc.detail * static_cast<f64>(chunk_extent(c.level))) {
            for (const ChunkCoord child : children(c)) {
                stack.push_back(child);
            }
        } else {
            leaves.insert(c);
        }
    }

    // Ausgleichen: Ein Blatt, das neben einem mehr als eine Stufe feineren liegt, teilt sich.
    // Seine Kinder decken denselben Bereich, also auch über die Sichtweite hinaus.
    LodLayout layout{std::vector<ChunkCoord>{leaves.begin(), leaves.end()}};
    for (bool changed = true; changed;) {
        changed = false;
        for (const ChunkCoord leaf : layout.leaves()) {
            const SampleBox b = chunk_box(leaf);
            const i64 le = chunk_extent(leaf.level);
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dy = -1; dy <= 1; ++dy) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const auto other =
                            layout.leaf_at2(probe2(b.min_x, le, dx), probe2(b.min_y, le, dy),
                                            probe2(b.min_z, le, dz));
                        if (other && other->level > leaf.level + 1 && leaves.erase(*other) > 0) {
                            for (const ChunkCoord child : children(*other)) {
                                if (in_height(child)) {
                                    leaves.insert(child);
                                }
                            }
                            changed = true;
                        }
                    }
                }
            }
        }
        if (changed) {
            layout = LodLayout{std::vector<ChunkCoord>{leaves.begin(), leaves.end()}};
        }
    }
    return layout;
}

void mesh_leaves(jobs::JobSystem& jobs, const LodLayout& layout, const ChunkStore& store,
                 std::span<const ChunkCoord> leaves, std::span<Mesh> out) {
    NGINE_PROFILE_ZONE("voxel::mesh_leaves");
    verify(out.size() == leaves.size(), "voxel::mesh_leaves: out passt nicht zu leaves");
    jobs.parallel_for(leaves.size(), 1, [&](usize begin, usize end) {
        LodMesher mesher;
        for (usize i = begin; i < end; ++i) {
            mesher.build(layout, store, leaves[i], out[i]);
        }
    });
}

std::optional<RayHit> raycast(const LodLayout& layout, const ChunkStore& store, Vec3 origin,
                              Vec3 direction, f32 max_length) {
    // Abstand in Voxeln am Punkt p und Schrittweite der Stufe dort.
    const auto distance_at = [&](Vec3 p, f32& spacing) -> f32 {
        spacing = 1.0f;
        const auto x = static_cast<i64>(std::floor(p.x));
        const auto y = static_cast<i64>(std::floor(p.y));
        const auto z = static_cast<i64>(std::floor(p.z));
        const std::optional<ChunkCoord> leaf = layout.leaf_of(x, y, z);
        if (!leaf) {
            return -max_distance;
        }
        if (leaf->level == 0) {
            return store.distance_at(p);
        }
        const Chunk* chunk = store.find(*leaf);
        if (chunk == nullptr) {
            return -max_distance;
        }
        spacing = static_cast<f32>(i64{1} << leaf->level);
        const SampleBox box = chunk_box(*leaf);
        const auto local = [&](f32 v, i64 min) {
            return std::clamp((v - static_cast<f32>(min)) / spacing, 0.0f,
                              static_cast<f32>(chunk_voxels - 1));
        };
        const f32 lx = local(p.x, box.min_x);
        const f32 ly = local(p.y, box.min_y);
        const f32 lz = local(p.z, box.min_z);
        const auto ix = std::min(static_cast<u32>(lx), u32{chunk_voxels - 2});
        const auto iy = std::min(static_cast<u32>(ly), u32{chunk_voxels - 2});
        const auto iz = std::min(static_cast<u32>(lz), u32{chunk_voxels - 2});
        const f32 tx = lx - static_cast<f32>(ix);
        const f32 ty = ly - static_cast<f32>(iy);
        const f32 tz = lz - static_cast<f32>(iz);
        const auto d = [&](u32 dx, u32 dy, u32 dz) {
            return dequantize(chunk->sample(ix + dx, iy + dy, iz + dz).density);
        };
        const auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
        const f32 c00 = lerp(d(0, 0, 0), d(1, 0, 0), tx);
        const f32 c10 = lerp(d(0, 1, 0), d(1, 1, 0), tx);
        const f32 c01 = lerp(d(0, 0, 1), d(1, 0, 1), tx);
        const f32 c11 = lerp(d(0, 1, 1), d(1, 1, 1), tx);
        return lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz) * spacing;
    };
    f32 spacing = 1.0f;
    f32 previous_t = 0.0f;
    f32 previous_d = distance_at(origin, spacing);
    for (f32 t = 0.25f * spacing; t <= max_length; t += 0.25f * spacing) {
        const Vec3 p = origin + direction * t;
        const f32 d = distance_at(p, spacing);
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
