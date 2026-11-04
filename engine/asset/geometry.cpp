module;

#include <cmath>
#include <span>
#include <vector>

module ngine.asset;

import ngine.core;
import ngine.math;

namespace ngine::asset {

namespace {

using math::Vec2;
using math::Vec3;

// Beliebige Einheitssenkrechte zu n.
Vec3 any_perpendicular(Vec3 n) {
    const Vec3 axis = std::abs(n.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    return math::normalize(math::cross(axis, n));
}

bool valid_triangle(std::span<const u32> indices, usize t, usize count) {
    return indices[t] < count && indices[t + 1] < count && indices[t + 2] < count;
}

} // namespace

void generate_normals(std::span<Vertex> vertices, std::span<const u32> indices) {
    std::vector<Vec3> sums(vertices.size(), Vec3{});
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        if (!valid_triangle(indices, t, vertices.size())) {
            continue;
        }
        const u32 a = indices[t];
        const u32 b = indices[t + 1];
        const u32 c = indices[t + 2];
        // Das Kreuzprodukt ist doppelt so lang wie die Fläche: Große Dreiecke zählen mehr.
        const Vec3 n = math::cross(vertices[b].position - vertices[a].position,
                                   vertices[c].position - vertices[a].position);
        sums[a] = sums[a] + n;
        sums[b] = sums[b] + n;
        sums[c] = sums[c] + n;
    }
    for (usize i = 0; i < vertices.size(); ++i) {
        const Vec3 n = math::normalize(sums[i]);
        vertices[i].normal = math::length_squared(n) > 0.0f ? n : Vec3{0.0f, 1.0f, 0.0f};
    }
}

void generate_tangents(std::span<Vertex> vertices, std::span<const u32> indices) {
    std::vector<Vec3> tangents(vertices.size(), Vec3{});
    std::vector<Vec3> bitangents(vertices.size(), Vec3{});
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        if (!valid_triangle(indices, t, vertices.size())) {
            continue;
        }
        const u32 i[3] = {indices[t], indices[t + 1], indices[t + 2]};
        const Vec3 e1 = vertices[i[1]].position - vertices[i[0]].position;
        const Vec3 e2 = vertices[i[2]].position - vertices[i[0]].position;
        const Vec2 d1 = vertices[i[1]].uv - vertices[i[0]].uv;
        const Vec2 d2 = vertices[i[2]].uv - vertices[i[0]].uv;
        const f32 det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) {
            continue; // Texturkoordinaten ohne Fläche: Tangente unbestimmt
        }
        const f32 r = 1.0f / det;
        const Vec3 tangent = (e1 * d2.y - e2 * d1.y) * r;
        const Vec3 bitangent = (e2 * d1.x - e1 * d2.x) * r;
        for (const u32 v : i) {
            tangents[v] = tangents[v] + tangent;
            bitangents[v] = bitangents[v] + bitangent;
        }
    }
    for (usize v = 0; v < vertices.size(); ++v) {
        const Vec3 n = vertices[v].normal;
        // Gram-Schmidt: Anteil in Richtung der Normale entfernen.
        Vec3 tangent = math::normalize(tangents[v] - n * math::dot(n, tangents[v]));
        if (math::length_squared(tangent) == 0.0f) {
            tangent = any_perpendicular(n);
        }
        const f32 sign = math::dot(math::cross(n, tangent), bitangents[v]) < 0.0f ? -1.0f : 1.0f;
        vertices[v].tangent = {tangent.x, tangent.y, tangent.z, sign};
    }
}

} // namespace ngine::asset
