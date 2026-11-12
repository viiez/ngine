module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

module ngine.meshlet;

import ngine.core;
import ngine.math;

namespace ngine::meshlet {

namespace {

using math::Vec3;

} // namespace

Vec3 Positions::operator[](usize index) const noexcept {
    std::array<f32, 3> xyz{};
    std::memcpy(xyz.data(), data + index * stride, sizeof(xyz));
    return {xyz[0], xyz[1], xyz[2]};
}

std::vector<Meshlet> build(Positions positions, std::span<u32> indices) {
    const usize triangle_count = indices.size() / 3;
    std::vector<Meshlet> meshlets;
    if (triangle_count == 0) {
        return meshlets;
    }

    std::vector<Vec3> centers(triangle_count);
    for (usize t = 0; t < triangle_count; ++t) {
        centers[t] = (positions[indices[t * 3]] + positions[indices[t * 3 + 1]] +
                      positions[indices[t * 3 + 2]]) *
                     (1.0f / 3.0f);
    }
    std::vector<u32> order(triangle_count);
    std::iota(order.begin(), order.end(), 0u);

    // Passen die Dreiecke [first, last) der Reihenfolge in ein Meshlet?
    std::vector<u32> local;
    local.reserve(max_vertices + 3);
    const auto fits = [&](usize first, usize last) {
        if (last - first > max_triangles) {
            return false;
        }
        local.clear();
        for (usize i = first; i < last; ++i) {
            for (usize corner = 0; corner < 3; ++corner) {
                const u32 vertex = indices[order[i] * 3 + corner];
                if (std::ranges::find(local, vertex) == local.end()) {
                    local.push_back(vertex);
                    if (local.size() > max_vertices) {
                        return false;
                    }
                }
            }
        }
        return true;
    };

    // Teilen wie ein k-d-Baum: Ein Bereich, der nicht passt, wird am Median entlang der längsten
    // Achse seiner Mittelpunkte halbiert. So bleiben Meshlets räumlich kompakt. Bei gleichen
    // Mittelpunkten entscheidet die Nummer des Dreiecks, damit das Ergebnis überall gleich ist.
    std::vector<std::pair<usize, usize>> ranges;
    std::vector<std::pair<usize, usize>> stack{{0, triangle_count}};
    while (!stack.empty()) {
        const auto [first, last] = stack.back();
        stack.pop_back();
        if (fits(first, last)) {
            ranges.emplace_back(first, last);
            continue;
        }
        Vec3 low = centers[order[first]];
        Vec3 high = low;
        for (usize i = first + 1; i < last; ++i) {
            low = math::min(low, centers[order[i]]);
            high = math::max(high, centers[order[i]]);
        }
        const Vec3 extent = high - low;
        const usize axis =
            extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
        const usize middle = first + (last - first) / 2;
        std::nth_element(order.begin() + static_cast<std::ptrdiff_t>(first),
                         order.begin() + static_cast<std::ptrdiff_t>(middle),
                         order.begin() + static_cast<std::ptrdiff_t>(last), [&](u32 a, u32 b) {
                             const f32 ca = centers[a][axis];
                             const f32 cb = centers[b][axis];
                             return ca < cb || (ca == cb && a < b);
                         });
        // Erst die obere Hälfte auf den Stapel: Die untere kommt zuerst dran, die Reihenfolge der
        // Meshlets folgt dem Baum.
        stack.emplace_back(middle, last);
        stack.emplace_back(first, middle);
    }

    std::vector<u32> sorted;
    sorted.reserve(indices.size());
    meshlets.reserve(ranges.size());
    for (const auto& [first, last] : ranges) {
        static_cast<void>(fits(first, last));
        meshlets.push_back({.first_triangle = static_cast<u32>(sorted.size() / 3),
                            .triangle_count = static_cast<u32>(last - first),
                            .vertex_count = static_cast<u32>(local.size())});
        for (usize i = first; i < last; ++i) {
            const u32 t = order[i];
            sorted.insert(sorted.end(), {indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2]});
        }
    }
    std::ranges::copy(sorted, indices.begin());
    for (Meshlet& meshlet : meshlets) {
        compute_bounds(positions, indices, meshlet);
    }
    return meshlets;
}

void compute_bounds(Positions positions, std::span<const u32> indices, Meshlet& meshlet) {
    // Kugel um die achsenparallele Hülle der Eckpunkte.
    Vec3 low{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
             std::numeric_limits<f32>::max()};
    Vec3 high{-low.x, -low.y, -low.z};
    const usize first = usize{meshlet.first_triangle} * 3;
    const usize end = first + usize{meshlet.triangle_count} * 3;
    for (usize i = first; i < end; ++i) {
        const Vec3 p = positions[indices[i]];
        low = math::min(low, p);
        high = math::max(high, p);
    }
    if (meshlet.triangle_count == 0) {
        meshlet.center = {};
        meshlet.radius = 0.0f;
        meshlet.cone_axis = {};
        meshlet.cone_cutoff = 1.0f;
        return;
    }
    const Vec3 center = (low + high) * 0.5f;
    f32 radius = 0.0f;
    for (usize i = first; i < end; ++i) {
        radius = std::max(radius, math::length(positions[indices[i]] - center));
    }
    meshlet.center = center;
    // Etwas Spielraum gegen Rundung beim Prüfen auf der GPU.
    meshlet.radius = radius * 1.0001f + 1e-6f;

    // Kegel: mittlere Richtung der Dreiecksnormalen und der größte Winkel dazu.
    std::vector<Vec3> normals;
    normals.reserve(meshlet.triangle_count);
    Vec3 sum{};
    for (usize i = first; i < end; i += 3) {
        const Vec3 a = positions[indices[i]];
        const Vec3 n = math::cross(positions[indices[i + 1]] - a, positions[indices[i + 2]] - a);
        const f32 len = math::length(n);
        if (len <= 0.0f || !std::isfinite(len)) {
            continue;
        }
        const Vec3 unit = n * (1.0f / len);
        normals.push_back(unit);
        sum = sum + unit;
    }
    meshlet.cone_axis = {};
    meshlet.cone_cutoff = 1.0f;
    const f32 sum_length = math::length(sum);
    if (normals.empty() || sum_length <= 1e-6f) {
        return;
    }
    const Vec3 axis = sum * (1.0f / sum_length);
    f32 min_dot = 1.0f;
    for (const Vec3& n : normals) {
        min_dot = std::min(min_dot, math::dot(n, axis));
    }
    // Ab etwa 84 Grad Öffnung lohnt der Test nicht mehr; er sortiert dann kaum etwas aus.
    if (min_dot <= 0.1f) {
        return;
    }
    meshlet.cone_axis = axis;
    // sin des halben Öffnungswinkels, aufgerundet gegen Rundung.
    meshlet.cone_cutoff = std::min(std::sqrt(1.0f - min_dot * min_dot) + 1e-3f, 1.0f);
}

bool valid(std::span<const Meshlet> meshlets, std::span<const u32> indices) {
    u32 next = 0;
    std::vector<u32> local;
    for (const Meshlet& meshlet : meshlets) {
        if (meshlet.first_triangle != next || meshlet.triangle_count == 0 ||
            meshlet.triangle_count > max_triangles || meshlet.vertex_count > max_vertices) {
            return false;
        }
        local.clear();
        const usize first = usize{meshlet.first_triangle} * 3;
        const usize end = first + usize{meshlet.triangle_count} * 3;
        if (end > indices.size()) {
            return false;
        }
        for (usize i = first; i < end; ++i) {
            if (std::ranges::find(local, indices[i]) == local.end()) {
                local.push_back(indices[i]);
            }
        }
        if (local.size() != meshlet.vertex_count) {
            return false;
        }
        next += meshlet.triangle_count;
    }
    return usize{next} * 3 == indices.size();
}

LocalData local_data(std::span<const Meshlet> meshlets, std::span<const u32> indices) {
    LocalData data;
    data.vertex_offsets.reserve(meshlets.size());
    data.triangles.reserve(indices.size() / 3);
    std::vector<u32> local;
    for (const Meshlet& meshlet : meshlets) {
        data.vertex_offsets.push_back(static_cast<u32>(data.vertices.size()));
        local.clear();
        const usize first = usize{meshlet.first_triangle} * 3;
        for (usize t = 0; t < meshlet.triangle_count; ++t) {
            u32 packed = 0;
            for (usize corner = 0; corner < 3; ++corner) {
                const u32 vertex = indices[first + t * 3 + corner];
                auto found = std::ranges::find(local, vertex);
                if (found == local.end()) {
                    local.push_back(vertex);
                    found = local.end() - 1;
                }
                packed |= static_cast<u32>(found - local.begin()) << (corner * 8);
            }
            data.triangles.push_back(packed);
        }
        data.vertices.insert(data.vertices.end(), local.begin(), local.end());
    }
    return data;
}

} // namespace ngine::meshlet
