#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

import ngine.core;
import ngine.math;
import ngine.voxel;

using namespace ngine;
using namespace ngine::math;
using namespace ngine::voxel;

namespace {

// Kugel mit Radius r um die Mitte eines Gitters mit n Samples pro Achse.
Grid sphere_grid(u32 n, f32 r, Material material = 2) {
    Grid grid{{n, n, n}};
    const f32 c = static_cast<f32>(n - 1) * 0.5f;
    fill(
        grid, [&](Vec3 p) { return r - length(p - Vec3{c, c, c}); },
        [&](Vec3) { return material; });
    return grid;
}

Vec3 position(const Vertex& v) {
    return {v.px, v.py, v.pz};
}
Vec3 normal(const Vertex& v) {
    return {v.nx, v.ny, v.nz};
}

} // namespace

TEST_CASE("Dichte wird mit 8 Bit quantisiert", "[voxel]") {
    CHECK(quantize(0.0f) == 0);
    CHECK(quantize(1.0f) == 32);
    CHECK(quantize(-1.0f) == -32);
    CHECK(quantize(100.0f) == 127);
    CHECK(quantize(-100.0f) == -127);
    CHECK(dequantize(quantize(0.5f)) == Catch::Approx(0.5f));
    // Neue Samples sind Luft.
    const Grid grid{{4, 4, 4}};
    CHECK(grid.sample(1, 2, 3).material == air);
    CHECK(grid.distance(1, 2, 3) < 0.0f);
}

TEST_CASE("Trilineare Interpolation und Gradient", "[voxel]") {
    Grid grid{{8, 8, 8}};
    // Ebene y = 3.5, darunter fest: Abstand 3.5 - y.
    fill(grid, [](Vec3 p) { return 3.5f - p.y; }, [](Vec3) { return Material{1}; });
    CHECK(grid.distance_at({2.0f, 3.5f, 2.0f}) == Catch::Approx(0.0f).margin(0.02));
    CHECK(grid.distance_at({2.3f, 1.25f, 4.7f}) == Catch::Approx(2.25f).margin(0.02));
    const Vec3 g = grid.gradient_at({3.0f, 3.5f, 3.0f});
    CHECK(g.x == Catch::Approx(0.0f).margin(1e-4));
    CHECK(g.y == Catch::Approx(-1.0f).margin(0.02));
    CHECK(g.z == Catch::Approx(0.0f).margin(1e-4));
    // Außerhalb wird geklemmt.
    CHECK(grid.distance_at({-5.0f, 0.0f, 0.0f}) == Catch::Approx(3.5f).margin(0.02));
}

TEST_CASE("Leeres und volles Gitter ergeben kein Netz", "[voxel]") {
    Mesher mesher;
    Mesh mesh;
    Grid grid{{9, 9, 9}};
    mesher.build(grid, mesh);
    CHECK(mesh.vertices.empty());
    CHECK(mesh.indices.empty());

    fill(grid, [](Vec3) { return 2.0f; }, [](Vec3) { return Material{1}; });
    mesher.build(grid, mesh);
    CHECK(mesh.vertices.empty());
}

TEST_CASE("Surface Nets erzeugen eine geschlossene Kugel", "[voxel]") {
    const f32 radius = 6.0f;
    const Grid grid = sphere_grid(17, radius);
    Mesher mesher;
    Mesh mesh;
    mesher.build(grid, mesh);

    REQUIRE(mesh.vertices.size() > 100);
    REQUIRE(mesh.indices.size() % 3 == 0);
    for (const u32 i : mesh.indices) {
        REQUIRE(i < mesh.vertices.size());
    }

    const Vec3 center{8.0f, 8.0f, 8.0f};
    f32 max_error = 0.0f;
    for (const Vertex& v : mesh.vertices) {
        // Eckpunkte liegen auf der Kugel, trotz 8-Bit-Dichte nur wenig daneben.
        max_error = std::max(max_error, std::abs(length(position(v) - center) - radius));
        // Normalen sind normiert und zeigen nach außen.
        CHECK(length(normal(v)) == Catch::Approx(1.0f).margin(1e-3));
        CHECK(dot(normal(v), normalize(position(v) - center)) > 0.95f);
        CHECK(mesh.dominant(v) == 2);
        CHECK(mesh.weight(v, 2) == 255);
    }
    INFO("Größter Abstand zur Kugel: " << max_error << " Voxel");
    CHECK(max_error < 0.15f);

    // Jedes Dreieck liegt gegen den Uhrzeigersinn von außen gesehen.
    for (usize t = 0; t < mesh.indices.size(); t += 3) {
        const Vec3 a = position(mesh.vertices[mesh.indices[t]]);
        const Vec3 b = position(mesh.vertices[mesh.indices[t + 1]]);
        const Vec3 c = position(mesh.vertices[mesh.indices[t + 2]]);
        const Vec3 face = cross(b - a, c - a);
        CHECK(dot(face, (a + b + c) * (1.0f / 3.0f) - center) > 0.0f);
    }

    // Geschlossen: Jede Kante gehört zu genau zwei Dreiecken, einmal in jeder Richtung.
    std::map<std::pair<u32, u32>, int> edges;
    for (usize t = 0; t < mesh.indices.size(); t += 3) {
        for (usize e = 0; e < 3; ++e) {
            const u32 from = mesh.indices[t + e];
            const u32 to = mesh.indices[t + (e + 1) % 3];
            ++edges[{from, to}];
        }
    }
    bool closed = true;
    for (const auto& [edge, count] : edges) {
        closed = closed && count == 1 && edges.contains({edge.second, edge.first});
    }
    CHECK(closed);
}

TEST_CASE("Der Mesher verwendet seinen Speicher wieder", "[voxel]") {
    const Grid grid = sphere_grid(17, 5.0f);
    Mesher mesher;
    Mesh mesh;
    mesher.build(grid, mesh);
    const auto vertices = mesh.vertices.size();
    const auto* data = mesh.vertices.data();
    mesher.build(grid, mesh);
    CHECK(mesh.vertices.size() == vertices);
    CHECK(mesh.vertices.data() == data);
}

TEST_CASE("Der Huegel hat Gras an der Oberflaeche und Erde darunter", "[voxel]") {
    Grid grid{{33, 33, 33}};
    fill_hill(grid);
    CHECK(grid.version() == 1);
    // Mitte: fest bis zur Spitze (6 + 14 = 20), darüber Luft.
    CHECK(grid.distance(16, 19, 16) > 0.0f);
    CHECK(grid.distance(16, 21, 16) < 0.0f);
    CHECK(grid.sample(16, 19, 16).material == 1);
    CHECK(grid.sample(16, 10, 16).material == 3);
    CHECK(grid.sample(16, 25, 16).material == air);
    // Rand: nur die Grundfläche.
    CHECK(grid.distance(0, 5, 0) > 0.0f);
    CHECK(grid.distance(0, 7, 0) < 0.0f);

    // Das Netz zeigt nur die Oberfläche, also nur Gras.
    Mesher mesher;
    Mesh mesh;
    mesher.build(grid, mesh);
    REQUIRE_FALSE(mesh.vertices.empty());
    CHECK(
        std::ranges::all_of(mesh.vertices, [&](const Vertex& v) { return mesh.dominant(v) == 1; }));

    // Nach dem Abtragen kommt Erde zum Vorschein.
    apply(grid, {.mode = BrushMode::Remove, .center = {16, 20, 16}, .radius = 6.0f});
    mesher.build(grid, mesh);
    CHECK(std::ranges::any_of(mesh.vertices,
                              [&](const Vertex& v) { return mesh.weight(v, 3) > 128; }));
}

TEST_CASE("Pinsel tragen ab, tragen auf und glaetten", "[voxel]") {
    Grid grid = sphere_grid(17, 6.0f);
    const u64 version = grid.version();

    SECTION("Abtragen") {
        const Box box =
            apply(grid, {.mode = BrushMode::Remove, .center = {8, 14, 8}, .radius = 3.0f});
        CHECK_FALSE(box.empty());
        CHECK(grid.version() == version + 1);
        CHECK(grid.distance(8, 13, 8) < 0.0f);
        CHECK(grid.sample(8, 13, 8).material == air);
        CHECK(grid.distance(8, 8, 8) > 0.0f);
    }
    SECTION("Auftragen") {
        apply(grid, {.mode = BrushMode::Add, .center = {8, 15, 8}, .radius = 2.0f, .material = 3});
        CHECK(grid.distance(8, 15, 8) > 0.0f);
        CHECK(grid.sample(8, 15, 8).material == 3);
        // Bestehender Stoff behält sein Material.
        CHECK(grid.sample(8, 8, 8).material == 2);
    }
    SECTION("Glaetten verkleinert scharfe Kanten") {
        Grid cube{{17, 17, 17}};
        fill(
            cube,
            [](Vec3 p) {
                const f32 d = std::max({std::abs(p.x - 8), std::abs(p.y - 8), std::abs(p.z - 8)});
                return 4.0f - d;
            },
            [](Vec3) { return Material{2}; });
        const f32 corner_before = cube.distance(12, 12, 12);
        apply(
            cube,
            {.mode = BrushMode::Smooth, .center = {12, 12, 12}, .radius = 3.0f, .strength = 1.0f});
        CHECK(cube.distance(12, 12, 12) < corner_before);
        CHECK(cube.distance(8, 8, 8) == Catch::Approx(4.0f).margin(0.05));
    }
    SECTION("Pinsel ausserhalb aendert nichts") {
        const Box box =
            apply(grid, {.mode = BrushMode::Remove, .center = {100, 100, 100}, .radius = 2.0f});
        CHECK(box.empty());
        CHECK(grid.version() == version);
    }
}

TEST_CASE("Strahltest trifft die Oberflaeche", "[voxel]") {
    const Grid grid = sphere_grid(17, 6.0f);
    const auto hit = raycast(grid, {8.0f, 30.0f, 8.0f}, {0.0f, -1.0f, 0.0f}, 50.0f);
    REQUIRE(hit);
    CHECK(hit->position.y == Catch::Approx(14.0f).margin(0.1));
    CHECK(hit->distance == Catch::Approx(16.0f).margin(0.1));

    CHECK_FALSE(raycast(grid, {8.0f, 30.0f, 8.0f}, {0.0f, 1.0f, 0.0f}, 50.0f));
    CHECK_FALSE(raycast(grid, {0.0f, 30.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 5.0f));
}
