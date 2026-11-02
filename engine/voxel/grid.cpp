module;

#include <algorithm>
#include <cmath>

module ngine.voxel;

import ngine.core;

namespace ngine::voxel {

i8 quantize(f32 distance) noexcept {
    const f32 scaled = std::round(distance * density_scale);
    return static_cast<i8>(std::clamp(scaled, -127.0f, 127.0f));
}

Grid::Grid(Extent3 size) : size_{size}, samples_(size.count()) {
    verify(size.x >= 2 && size.y >= 2 && size.z >= 2,
           "Ein Gitter braucht mindestens 2 Samples pro Achse");
}

namespace {

// Teilt eine Koordinate in Zelle und Anteil, geklemmt an das Gitter.
struct Axis {
    u32 i0;
    u32 i1;
    f32 t;
};

Axis split(f32 v, u32 size) noexcept {
    const f32 max = static_cast<f32>(size - 1);
    const f32 c = std::clamp(v, 0.0f, max);
    const auto i0 = std::min(static_cast<u32>(c), size - 2);
    return {i0, i0 + 1, c - static_cast<f32>(i0)};
}

f32 lerp(f32 a, f32 b, f32 t) noexcept {
    return a + (b - a) * t;
}

} // namespace

f32 Grid::distance_at(Vec3 p) const noexcept {
    const Axis ax = split(p.x, size_.x);
    const Axis ay = split(p.y, size_.y);
    const Axis az = split(p.z, size_.z);
    const f32 c00 = lerp(distance(ax.i0, ay.i0, az.i0), distance(ax.i1, ay.i0, az.i0), ax.t);
    const f32 c10 = lerp(distance(ax.i0, ay.i1, az.i0), distance(ax.i1, ay.i1, az.i0), ax.t);
    const f32 c01 = lerp(distance(ax.i0, ay.i0, az.i1), distance(ax.i1, ay.i0, az.i1), ax.t);
    const f32 c11 = lerp(distance(ax.i0, ay.i1, az.i1), distance(ax.i1, ay.i1, az.i1), ax.t);
    return lerp(lerp(c00, c10, ay.t), lerp(c01, c11, ay.t), az.t);
}

Vec3 Grid::gradient_at(Vec3 p) const noexcept {
    // Zentrale Differenzen über eine Zelle Abstand; trilinear interpoliert, damit die Normalen
    // stetig über die Zellgrenzen laufen.
    constexpr f32 h = 1.0f;
    return {
        (distance_at(p + Vec3{h, 0, 0}) - distance_at(p - Vec3{h, 0, 0})) * (0.5f / h),
        (distance_at(p + Vec3{0, h, 0}) - distance_at(p - Vec3{0, h, 0})) * (0.5f / h),
        (distance_at(p + Vec3{0, 0, h}) - distance_at(p - Vec3{0, 0, h})) * (0.5f / h),
    };
}

void fill(Grid& grid, const DistanceFunction& distance, const MaterialFunction& material) {
    const Extent3 size = grid.size();
    for (u32 z = 0; z < size.z; ++z) {
        for (u32 y = 0; y < size.y; ++y) {
            for (u32 x = 0; x < size.x; ++x) {
                const Vec3 p{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)};
                const i8 density = quantize(distance(p));
                grid.set(x, y, z, {density, density > 0 ? material(p) : air});
            }
        }
    }
    grid.touch();
}

void fill_hill(Grid& grid, const HillDesc& desc) {
    const Extent3 size = grid.size();
    const f32 cx = static_cast<f32>(size.x - 1) * 0.5f;
    const f32 cz = static_cast<f32>(size.z - 1) * 0.5f;
    const f32 inv_r2 = 1.0f / (desc.radius * desc.radius);

    // Höhe als Glocke über der Grundfläche. Der Abstand ist näherungsweise die Höhe über dem
    // Punkt, geteilt durch die Länge des Gradienten der Höhenfunktion; so bleibt er auch an
    // steilen Hängen nah am echten Abstand.
    auto surface = [=](Vec3 p, f32& slope) {
        const f32 dx = p.x - cx;
        const f32 dz = p.z - cz;
        const f32 bell = std::exp(-(dx * dx + dz * dz) * inv_r2);
        const f32 h = desc.ground + desc.height * bell;
        const f32 gx = -2.0f * dx * inv_r2 * desc.height * bell;
        const f32 gz = -2.0f * dz * inv_r2 * desc.height * bell;
        slope = std::sqrt(gx * gx + gz * gz);
        return h;
    };
    auto distance = [&](Vec3 p) {
        f32 slope = 0.0f;
        const f32 h = surface(p, slope);
        return (h - p.y) / std::sqrt(1.0f + slope * slope);
    };
    // Gras an der Oberfläche, Erde darunter. Fels an steilen Hängen zeigt der Shader über die
    // Normale; so folgt er auch Änderungen, ohne dass das Material neu berechnet werden muss.
    fill(grid, distance,
         [&](Vec3 p) -> Material { return distance(p) > 2.5f ? Material{3} : Material{1}; });
}

} // namespace ngine::voxel
