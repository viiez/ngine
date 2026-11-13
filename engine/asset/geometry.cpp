module;

#include <cmath>
#include <span>
#include <vector>

module ngine.asset;

import ngine.core;
import ngine.math;
import ngine.meshlet;

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

Primitive make_sphere(f32 radius, u32 segments, u32 rings) {
    segments = segments < 3 ? 3 : segments;
    rings = rings < 2 ? 2 : rings;
    constexpr f32 pi = 3.14159265358979f;
    Primitive sphere;
    // Eine Spalte mehr als Segmente: Die Naht hat doppelte Eckpunkte mit u = 0 und u = 1.
    for (u32 ring = 0; ring <= rings; ++ring) {
        const f32 v = static_cast<f32>(ring) / static_cast<f32>(rings);
        const f32 theta = v * pi;
        for (u32 segment = 0; segment <= segments; ++segment) {
            const f32 u = static_cast<f32>(segment) / static_cast<f32>(segments);
            const f32 phi = u * 2.0f * pi;
            const Vec3 normal{std::sin(theta) * std::sin(phi), std::cos(theta),
                              std::sin(theta) * std::cos(phi)};
            // Tangente in Richtung wachsender u, auch an den Polen wohldefiniert.
            const Vec3 tangent{std::cos(phi), 0.0f, -std::sin(phi)};
            sphere.vertices.push_back({.position = normal * radius,
                                       .normal = normal,
                                       .tangent = {tangent.x, tangent.y, tangent.z, 1.0f},
                                       .uv = {u, v}});
        }
    }
    const u32 columns = segments + 1;
    for (u32 ring = 0; ring < rings; ++ring) {
        for (u32 segment = 0; segment < segments; ++segment) {
            const u32 a = ring * columns + segment;
            const u32 b = a + columns;
            // a oben links, b unten links; von außen gegen den Uhrzeigersinn.
            if (ring != 0) {
                sphere.indices.insert(sphere.indices.end(), {a, b, a + 1});
            }
            if (ring != rings - 1) {
                sphere.indices.insert(sphere.indices.end(), {a + 1, b, b + 1});
            }
        }
    }
    return sphere;
}

Primitive make_box(Vec3 half_extents) {
    struct Face {
        Vec3 normal;
        Vec3 u;
        Vec3 v;
    };
    // u und v spannen die Seite auf; cross(u, v) zeigt nach außen.
    constexpr Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    const auto scaled = [&](Vec3 d) {
        return Vec3{d.x * half_extents.x, d.y * half_extents.y, d.z * half_extents.z};
    };
    Primitive box;
    for (const Face& face : faces) {
        const u32 base = static_cast<u32>(box.vertices.size());
        constexpr f32 corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& corner : corners) {
            const Vec3 direction = face.normal + face.u * corner[0] + face.v * corner[1];
            box.vertices.push_back({.position = scaled(direction),
                                    .normal = face.normal,
                                    .tangent = {face.u.x, face.u.y, face.u.z, 1.0f},
                                    .uv = {corner[0] * 0.5f + 0.5f, 0.5f - corner[1] * 0.5f}});
        }
        box.indices.insert(box.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    generate_tangents(box.vertices, box.indices);
    return box;
}

void ensure_meshlets(Model& model) {
    for (Mesh& mesh : model.meshes) {
        for (Primitive& primitive : mesh.primitives) {
            if (primitive.meshlets.empty() && !primitive.indices.empty()) {
                primitive.meshlets = meshlet::build(
                    meshlet::positions_of(std::span<const Vertex>{primitive.vertices}),
                    primitive.indices);
            }
        }
    }
}

} // namespace ngine::asset
