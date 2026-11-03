module;

#include <array>
#include <limits>
#include <ngine/profile.hpp>
#include <span>

module ngine.voxel;

import ngine.core;
import ngine.math;
import ngine.profile;

namespace ngine::voxel {

using math::length;
using math::normalize;

namespace {

constexpr u32 no_vertex = std::numeric_limits<u32>::max();

// Die zwölf Kanten eines Voxels als Paare von Ecken. Ecke i liegt bei (i & 1, i >> 1 & 1, i >> 2 &
// 1).
constexpr std::array<std::array<u32, 2>, 12> cube_edges{{
    {0, 1},
    {2, 3},
    {4, 5},
    {6, 7}, // entlang x
    {0, 2},
    {1, 3},
    {4, 6},
    {5, 7}, // entlang y
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7}, // entlang z
}};

Vec3 corner_offset(u32 corner) {
    return {static_cast<f32>(corner & 1u), static_cast<f32>((corner >> 1) & 1u),
            static_cast<f32>((corner >> 2) & 1u)};
}

} // namespace

void Mesher::build(const Grid& grid, Mesh& out) {
    NGINE_PROFILE_ZONE("voxel::Mesher::build");
    out.clear();

    const Extent3 samples = grid.size();
    const Extent3 cells{samples.x - 1, samples.y - 1, samples.z - 1};
    cell_vertex_.assign(cells.count(), no_vertex);
    const usize cell_stride_y = cells.x;
    const usize cell_stride_z = usize{cells.x} * cells.y;

    // Abstände der acht Ecken eines Voxels im Sample-Array.
    const std::span<const Sample> data = grid.samples();
    const usize stride_y = samples.x;
    const usize stride_z = usize{samples.x} * samples.y;
    std::array<usize, 8> corner_offsets{};
    for (u32 c = 0; c < 8; ++c) {
        corner_offsets[c] = (c & 1u) + ((c >> 1) & 1u) * stride_y + ((c >> 2) & 1u) * stride_z;
    }

    auto emit_quad = [&](u32 v0, u32 v1, u32 v2, u32 v3, bool flip) {
        if (flip) {
            out.indices.insert(out.indices.end(), {v0, v3, v2, v0, v2, v1});
        } else {
            out.indices.insert(out.indices.end(), {v0, v1, v2, v0, v2, v3});
        }
    };

    // Ein Durchlauf über alle Voxel. Ein Voxel, das die Oberfläche schneidet, bekommt einen
    // Eckpunkt: das Mittel der Schnittpunkte auf seinen Kanten. Danach prüft es die drei Kanten an
    // seiner unteren Ecke; wechselt dort das Vorzeichen, entsteht ein Viereck aus den vier Voxeln
    // um die Kante. Die drei anderen liegen in Richtung kleinerer Koordinaten und haben ihren
    // Eckpunkt also schon. Kanten am Rand haben keine vier Voxel und bleiben offen (Chunk-Grenze,
    // ADR 0021).
    for (u32 z = 0; z < cells.z; ++z) {
        for (u32 y = 0; y < cells.y; ++y) {
            usize base = z * stride_z + y * stride_y;
            usize cell = z * cell_stride_z + y * cell_stride_y;
            for (u32 x = 0; x < cells.x; ++x, ++base, ++cell) {
                // Schneller Test nur mit den Vorzeichen der 8-Bit-Werte.
                u32 inside = 0;
                for (u32 c = 0; c < 8; ++c) {
                    inside |= (data[base + corner_offsets[c]].density > 0 ? 1u : 0u) << c;
                }
                if (inside == 0 || inside == 0xFF) {
                    continue;
                }

                std::array<f32, 8> d{};
                for (u32 c = 0; c < 8; ++c) {
                    d[c] = dequantize(data[base + corner_offsets[c]].density);
                }
                Vec3 sum{};
                u32 crossings = 0;
                for (const auto& [a, b] : cube_edges) {
                    if (((inside >> a) & 1u) == ((inside >> b) & 1u)) {
                        continue;
                    }
                    const f32 t = d[a] / (d[a] - d[b]);
                    sum = sum + corner_offset(a) + (corner_offset(b) - corner_offset(a)) * t;
                    ++crossings;
                }
                const Vec3 position =
                    Vec3{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)} +
                    sum * (1.0f / static_cast<f32>(crossings));

                // Material der festen Ecke, die der Oberfläche am nächsten liegt: Tiefere Ecken
                // gehören oft schon zum Material darunter (Erde unter Gras).
                u32 solid_corner = 8;
                for (u32 c = 0; c < 8; ++c) {
                    if (d[c] > 0.0f && (solid_corner == 8 || d[c] < d[solid_corner])) {
                        solid_corner = c;
                    }
                }
                const Material material = data[base + corner_offsets[solid_corner]].material;

                // Die Dichte steigt nach innen; die Normale zeigt nach außen.
                const Vec3 normal = normalize(grid.gradient_at(position) * -1.0f);
                const auto vertex = static_cast<u32>(out.vertices.size());
                cell_vertex_[cell] = vertex;
                out.vertices.push_back({position.x, position.y, position.z, material, normal.x,
                                        normal.y, normal.z, 0.0f});

                // Vierecke um die Kanten an Ecke 0. Reihenfolge der Voxel so, dass das Viereck
                // gegen den Uhrzeigersinn liegt, wenn fester Stoff am Anfang der Kante liegt.
                const bool solid = (inside & 1u) != 0;
                if (y > 0 && z > 0 && solid != ((inside & 2u) != 0)) { // Kante entlang x
                    emit_quad(cell_vertex_[cell - cell_stride_y - cell_stride_z],
                              cell_vertex_[cell - cell_stride_z], vertex,
                              cell_vertex_[cell - cell_stride_y], !solid);
                }
                if (x > 0 && z > 0 && solid != ((inside & 4u) != 0)) { // Kante entlang y
                    emit_quad(cell_vertex_[cell - 1 - cell_stride_z], cell_vertex_[cell - 1],
                              vertex, cell_vertex_[cell - cell_stride_z], !solid);
                }
                if (x > 0 && y > 0 && solid != ((inside & 16u) != 0)) { // Kante entlang z
                    emit_quad(cell_vertex_[cell - 1 - cell_stride_y],
                              cell_vertex_[cell - cell_stride_y], vertex, cell_vertex_[cell - 1],
                              !solid);
                }
            }
        }
    }
}

} // namespace ngine::voxel
