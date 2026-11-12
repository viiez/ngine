#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <span>
#include <tuple>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.meshlet;

// Meshlets (R3.5, ADR 0043): Aufteilung, Hüllen, Kegel und lokale Daten ohne GPU.

using namespace ngine;
using namespace ngine::math;

namespace {

struct Grid {
    std::vector<Vec3> vertices;
    std::vector<u32> indices;
};

// Ebene aus n x n Vierecken in der xz-Ebene, Vorderseiten zeigen nach oben (+y).
Grid plane(u32 n, f32 y = 0.0f) {
    Grid grid;
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            grid.vertices.push_back({static_cast<f32>(x), y, static_cast<f32>(z)});
        }
    }
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const u32 a = z * (n + 1) + x;
            const u32 b = a + 1;
            const u32 c = a + n + 1;
            const u32 d = c + 1;
            // Gegen den Uhrzeigersinn von oben gesehen.
            grid.indices.insert(grid.indices.end(), {a, c, b, b, c, d});
        }
    }
    return grid;
}

std::multiset<std::array<u32, 3>> triangles(std::span<const u32> indices) {
    std::multiset<std::array<u32, 3>> set;
    for (usize i = 0; i < indices.size(); i += 3) {
        set.insert({indices[i], indices[i + 1], indices[i + 2]});
    }
    return set;
}

bool backfacing(const meshlet::Meshlet& m, Vec3 viewer) {
    const Vec3 to = m.center - viewer;
    return dot(to, m.cone_axis) >= m.cone_cutoff * length(to) + m.radius;
}

} // namespace

TEST_CASE("Meshlets cover every triangle within the limits", "[meshlet]") {
    Grid grid = plane(40);
    const auto before = triangles(grid.indices);
    const auto meshlets =
        meshlet::build(meshlet::positions_of(std::span<const Vec3>{grid.vertices}), grid.indices);
    CHECK(meshlet::valid(meshlets, grid.indices));
    // Umsortiert, aber dieselben Dreiecke mit derselben Drehrichtung.
    CHECK(triangles(grid.indices) == before);
    // 3200 Dreiecke; in einem Gitter begrenzen die 64 Eckpunkte (etwa 7 x 7 Vierecke).
    CHECK(meshlets.size() >= 26);
    CHECK(meshlets.size() <= 80);
    for (const meshlet::Meshlet& m : meshlets) {
        CHECK(m.vertex_count <= meshlet::max_vertices);
        CHECK(m.triangle_count <= meshlet::max_triangles);
        // Räumlich kompakt: viel kleiner als die ganze Ebene (Diagonale 56).
        CHECK(m.radius < 8.0f);
    }
}

TEST_CASE("Meshlet bounds contain their vertices", "[meshlet]") {
    Grid grid = plane(20);
    const auto meshlets =
        meshlet::build(meshlet::positions_of(std::span<const Vec3>{grid.vertices}), grid.indices);
    for (const meshlet::Meshlet& m : meshlets) {
        for (u32 i = m.first_triangle * 3; i < (m.first_triangle + m.triangle_count) * 3; ++i) {
            CHECK(length(grid.vertices[grid.indices[i]] - m.center) <= m.radius);
        }
    }
}

TEST_CASE("Meshlet cones reject views from behind only", "[meshlet]") {
    Grid grid = plane(16);
    const auto meshlets =
        meshlet::build(meshlet::positions_of(std::span<const Vec3>{grid.vertices}), grid.indices);
    REQUIRE(!meshlets.empty());
    for (const meshlet::Meshlet& m : meshlets) {
        // Flach: alle Normalen gleich, enger Kegel nach oben.
        CHECK(m.cone_axis.y > 0.99f);
        CHECK(m.cone_cutoff < 0.1f);
        // Von oben sichtbar, von unten abgewandt.
        CHECK_FALSE(backfacing(m, {8.0f, 10.0f, 8.0f}));
        CHECK(backfacing(m, {8.0f, -10.0f, 8.0f}));
    }
}

TEST_CASE("Meshlets of a closed box never reject visible faces", "[meshlet]") {
    // Würfel mit 6 x 8 x 8 Vierecken: Meshlets über Kanten bekommen weite Kegel.
    std::vector<Vec3> vertices;
    std::vector<u32> indices;
    const u32 n = 8;
    for (u32 axis = 0; axis < 3; ++axis) {
        for (const f32 side : {-1.0f, 1.0f}) {
            const auto base = static_cast<u32>(vertices.size());
            for (u32 j = 0; j <= n; ++j) {
                for (u32 i = 0; i <= n; ++i) {
                    const f32 u = -1.0f + 2.0f * static_cast<f32>(i) / static_cast<f32>(n);
                    const f32 v = -1.0f + 2.0f * static_cast<f32>(j) / static_cast<f32>(n);
                    Vec3 p{};
                    p[axis] = side;
                    p[(axis + 1) % 3] = u;
                    p[(axis + 2) % 3] = v;
                    vertices.push_back(p);
                }
            }
            for (u32 j = 0; j < n; ++j) {
                for (u32 i = 0; i < n; ++i) {
                    const u32 a = base + j * (n + 1) + i;
                    const u32 b = a + 1;
                    const u32 c = a + n + 1;
                    const u32 d = c + 1;
                    if (side > 0.0f) {
                        indices.insert(indices.end(), {a, b, d, a, d, c});
                    } else {
                        indices.insert(indices.end(), {a, d, b, a, c, d});
                    }
                }
            }
        }
    }
    const auto meshlets =
        meshlet::build(meshlet::positions_of(std::span<const Vec3>{vertices}), indices);
    REQUIRE(meshlet::valid(meshlets, indices));
    // Für viele Blickpunkte: Ein Meshlet, das ein zugewandtes Dreieck hat, wird nie verworfen.
    for (const Vec3 viewer : {Vec3{5, 0.3f, 0.2f}, Vec3{-4, 3, 2}, Vec3{0.1f, -6, 0.4f},
                              Vec3{3, 3, 3}, Vec3{-2, -2, 5}}) {
        for (const meshlet::Meshlet& m : meshlets) {
            bool any_front = false;
            for (u32 t = m.first_triangle; t < m.first_triangle + m.triangle_count; ++t) {
                const Vec3 a = vertices[indices[t * 3]];
                const Vec3 normal =
                    cross(vertices[indices[t * 3 + 1]] - a, vertices[indices[t * 3 + 2]] - a);
                any_front = any_front || dot(normal, a - viewer) < 0.0f;
            }
            if (any_front) {
                CHECK_FALSE(backfacing(m, viewer));
            }
        }
    }
}

TEST_CASE("Meshlet local data rebuilds the triangles", "[meshlet]") {
    Grid grid = plane(30);
    const auto meshlets =
        meshlet::build(meshlet::positions_of(std::span<const Vec3>{grid.vertices}), grid.indices);
    const meshlet::LocalData local = meshlet::local_data(meshlets, grid.indices);
    REQUIRE(local.vertex_offsets.size() == meshlets.size());
    REQUIRE(local.triangles.size() == grid.indices.size() / 3);
    for (usize m = 0; m < meshlets.size(); ++m) {
        const meshlet::Meshlet& meshlet = meshlets[m];
        for (u32 t = 0; t < meshlet.triangle_count; ++t) {
            const u32 packed = local.triangles[meshlet.first_triangle + t];
            for (u32 corner = 0; corner < 3; ++corner) {
                const u32 slot = (packed >> (corner * 8)) & 0xFFu;
                REQUIRE(slot < meshlet.vertex_count);
                CHECK(local.vertices[local.vertex_offsets[m] + slot] ==
                      grid.indices[(meshlet.first_triangle + t) * 3 + corner]);
            }
        }
    }
}

TEST_CASE("Meshlets of an empty mesh", "[meshlet]") {
    std::vector<Vec3> vertices;
    std::vector<u32> indices;
    CHECK(meshlet::build(meshlet::positions_of(std::span<const Vec3>{vertices}), indices).empty());
    CHECK(meshlet::valid({}, indices));
}
