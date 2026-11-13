#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.meshlet;
import ngine.render;

// GPU-getriebenes Zeichnen (R3.5): Stumpfebenen, gepackte Meshlet-Daten und Größe des Hi-Z, ohne
// GPU geprüft.

using namespace ngine;
using namespace ngine::math;

namespace {

bool inside(const std::array<Vec4, 6>& planes, Vec3 point, f32 radius = 0.0f) {
    for (const Vec4& plane : planes) {
        if (dot(xyz(plane), point) + plane.w < -radius) {
            return false;
        }
    }
    return true;
}

struct Grid {
    std::vector<Vec3> vertices;
    std::vector<u32> indices;
};

Grid plane(u32 n) {
    Grid grid;
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            grid.vertices.push_back({static_cast<f32>(x), 0.0f, static_cast<f32>(z)});
        }
    }
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const u32 a = z * (n + 1) + x;
            const u32 c = a + n + 1;
            grid.indices.insert(grid.indices.end(), {a, c, a + 1, a + 1, c, c + 1});
        }
    }
    return grid;
}

render::GpuMeshlet record(std::span<const u32> words, usize index) {
    render::GpuMeshlet m;
    std::memcpy(&m, words.data() + index * (sizeof(m) / sizeof(u32)), sizeof(m));
    return m;
}

} // namespace

TEST_CASE("Frustum planes of the reverse-z camera", "[render][culling]") {
    const Mat4 projection = perspective_reverse_z(1.0f, 16.0f / 9.0f, 0.1f);
    const auto planes = render::frustum_planes(projection);
    for (const Vec4& plane : planes) {
        const bool unit = std::abs(length(xyz(plane)) - 1.0f) < 1e-4f;
        const bool open = length(xyz(plane)) == 0.0f && plane.w == 1.0f;
        CHECK((unit || open));
    }
    // Die Kamera blickt entlang -z.
    CHECK(inside(planes, {0.0f, 0.0f, -1.0f}));
    CHECK(inside(planes, {0.0f, 0.0f, -100000.0f}));
    CHECK_FALSE(inside(planes, {0.0f, 0.0f, 1.0f}));
    CHECK_FALSE(inside(planes, {0.0f, 0.0f, -0.05f}));
    CHECK_FALSE(inside(planes, {100.0f, 0.0f, -10.0f}));
    CHECK_FALSE(inside(planes, {0.0f, -100.0f, -10.0f}));
    // Eine Kugel, die in den Stumpf ragt, bleibt.
    CHECK(inside(planes, {100.0f, 0.0f, -10.0f}, 95.0f));
}

TEST_CASE("Frustum planes of an orthographic box", "[render][culling]") {
    const Mat4 box = orthographic(-2.0f, 2.0f, -3.0f, 3.0f, 1.0f, 11.0f);
    const auto planes = render::frustum_planes(box);
    CHECK(inside(planes, {0.0f, 0.0f, -5.0f}));
    CHECK(inside(planes, {1.9f, -2.9f, -10.9f}));
    CHECK_FALSE(inside(planes, {2.1f, 0.0f, -5.0f}));
    CHECK_FALSE(inside(planes, {0.0f, 3.1f, -5.0f}));
    CHECK_FALSE(inside(planes, {0.0f, 0.0f, -0.9f}));
    CHECK_FALSE(inside(planes, {0.0f, 0.0f, -11.1f}));
    CHECK(inside(planes, {0.0f, 0.0f, -11.5f}, 1.0f));
}

TEST_CASE("Packed meshlet data reproduces the triangles", "[render][culling]") {
    Grid grid = plane(24);
    const auto positions = meshlet::positions_of(std::span<const Vec3>{grid.vertices});
    const std::vector<meshlet::Meshlet> meshlets = meshlet::build(positions, grid.indices);
    REQUIRE(meshlets.size() > 4);
    // Zwei Teile wie zwei Teilnetze: das zweite mit verschobenen Eckpunkten.
    std::vector<u32> shifted = grid.indices;
    for (u32& index : shifted) {
        index += 1000;
    }
    const std::array parts{render::MeshletPart{.meshlets = meshlets, .indices = grid.indices},
                           render::MeshletPart{.meshlets = meshlets, .indices = shifted}};
    const std::vector<u32> words = render::pack_meshlets(parts);
    const usize records = meshlets.size() * 2;
    REQUIRE(words.size() > records * 12);

    for (usize p = 0; p < parts.size(); ++p) {
        for (usize i = 0; i < meshlets.size(); ++i) {
            const meshlet::Meshlet& source = meshlets[i];
            const render::GpuMeshlet m = record(words, p * meshlets.size() + i);
            CHECK((m.counts & 0xFF) == source.vertex_count);
            CHECK(((m.counts >> 8) & 0xFF) == source.triangle_count);
            CHECK(m.first_triangle == source.first_triangle);
            CHECK(m.sphere.w == source.radius);
            CHECK(m.cone.w == source.cone_cutoff);
            for (u32 t = 0; t < source.triangle_count; ++t) {
                const u32 packed = words[m.triangle_offset + t];
                for (u32 k = 0; k < 3; ++k) {
                    const u32 local = (packed >> (8 * k)) & 0xFF;
                    REQUIRE(local < source.vertex_count);
                    CHECK(words[m.vertex_offset + local] ==
                          parts[p].indices[(source.first_triangle + t) * 3 + k]);
                }
            }
        }
    }
}

TEST_CASE("Packed meshlet data uses precomputed local data", "[render][culling]") {
    Grid grid = plane(12);
    const auto positions = meshlet::positions_of(std::span<const Vec3>{grid.vertices});
    const std::vector<meshlet::Meshlet> meshlets = meshlet::build(positions, grid.indices);
    const meshlet::LocalData local = meshlet::local_data(meshlets, grid.indices);
    const std::array computed{render::MeshletPart{.meshlets = meshlets, .indices = grid.indices}};
    const std::array given{
        render::MeshletPart{.meshlets = meshlets, .indices = grid.indices, .local = &local}};
    CHECK(render::pack_meshlets(computed) == render::pack_meshlets(given));
}

TEST_CASE("Packed meshlet data of nothing is empty", "[render][culling]") {
    CHECK(render::pack_meshlets({}).empty());
}

TEST_CASE("Hi-Z size and levels", "[render][culling]") {
    CHECK(render::hiz_extent({1920, 1080}).width == 1024);
    CHECK(render::hiz_extent({1920, 1080}).height == 1024);
    CHECK(render::hiz_extent({800, 600}).width == 512);
    CHECK(render::hiz_extent({2560, 1440}).width == 2048);
    CHECK(render::hiz_extent({2560, 1440}).height == 1024);
    CHECK(render::hiz_extent({0, 0}).width == 1);
    CHECK(render::hiz_level_count({1024, 1024}) == 11);
    CHECK(render::hiz_level_count({2048, 1024}) == 12);
    CHECK(render::hiz_level_count({1, 1}) == 1);
    CHECK(render::hiz_level_count({1u << 20, 1}) == render::max_hiz_levels);
}
