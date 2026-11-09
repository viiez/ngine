#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.voxel;

// Chunks, Randschicht und Meshing in Jobs (R3.1, ADR 0038).

using namespace ngine;
using namespace ngine::math;
using namespace ngine::voxel;

namespace {

// Alle Chunks von low bis high (inklusive) in jeder Achse.
std::vector<ChunkCoord> chunk_range(i32 low, i32 high) {
    std::vector<ChunkCoord> coords;
    for (i32 z = low; z <= high; ++z) {
        for (i32 y = low; y <= high; ++y) {
            for (i32 x = low; x <= high; ++x) {
                coords.push_back({x, y, z, 0});
            }
        }
    }
    return coords;
}

f32 sphere(Vec3 p, Vec3 center, f32 radius) {
    return radius - length(p - center);
}

// Ein Dreiecksnetz in Koordinaten des Terrains, mit verschweißten Eckpunkten.
struct WorldMesh {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
};

// Fügt die Netze der Chunks zusammen. Eckpunkte in den Randzellen gibt es in zwei Chunks; sie
// liegen bis auf Rundung an derselben Stelle und werden verschweißt.
WorldMesh merge(const std::map<ChunkCoord, Mesh>& meshes) {
    WorldMesh result;
    std::map<std::array<i64, 3>, std::vector<u32>> buckets;
    auto weld = [&](Vec3 p) {
        const std::array<i64, 3> key{std::llround(p.x * 16.0f), std::llround(p.y * 16.0f),
                                     std::llround(p.z * 16.0f)};
        for (i64 dz = -1; dz <= 1; ++dz) {
            for (i64 dy = -1; dy <= 1; ++dy) {
                for (i64 dx = -1; dx <= 1; ++dx) {
                    const auto it = buckets.find({key[0] + dx, key[1] + dy, key[2] + dz});
                    if (it == buckets.end()) {
                        continue;
                    }
                    for (const u32 index : it->second) {
                        if (length(result.positions[index] - p) < 1e-3f) {
                            return index;
                        }
                    }
                }
            }
        }
        const auto index = static_cast<u32>(result.positions.size());
        result.positions.push_back(p);
        buckets[key].push_back(index);
        return index;
    };
    for (const auto& [coord, mesh] : meshes) {
        const Vec3 origin{static_cast<f32>(chunk_origin(coord.x)),
                          static_cast<f32>(chunk_origin(coord.y)),
                          static_cast<f32>(chunk_origin(coord.z))};
        std::vector<u32> remap;
        remap.reserve(mesh.vertices.size());
        for (const Vertex& v : mesh.vertices) {
            remap.push_back(weld(origin + Vec3{v.px, v.py, v.pz}));
        }
        for (const u32 index : mesh.indices) {
            result.indices.push_back(remap[index]);
        }
    }
    return result;
}

// Geschlossen: Jede gerichtete Kante kommt genau einmal vor, ihre Umkehrung auch.
bool closed(const WorldMesh& mesh) {
    std::map<std::pair<u32, u32>, int> edges;
    for (usize t = 0; t < mesh.indices.size(); t += 3) {
        for (usize e = 0; e < 3; ++e) {
            ++edges[{mesh.indices[t + e], mesh.indices[t + (e + 1) % 3]}];
        }
    }
    return std::ranges::all_of(edges, [&](const auto& entry) {
        return entry.second == 1 && edges.contains({entry.first.second, entry.first.first});
    });
}

// Dreiecke als Lagen ihrer Ecken (gerundet auf 1/256 Sample), jedes mit der kleinsten Ecke zuerst
// unter Erhalt des Umlaufsinns, sortiert. Zwei Netze mit derselben Liste haben dieselben Dreiecke,
// auch wenn mehrere Eckpunkte an derselben Stelle liegen (am Grund eines Kraters fallen Eckpunkte
// von Zellen um ein Sample mit Dichte 0 zusammen), wo Verschweißen nach Lage nicht hilft.
using Triangle = std::array<std::array<i64, 3>, 3>;

std::vector<Triangle> triangles(const std::vector<Vec3>& positions,
                                const std::vector<u32>& indices) {
    const auto key = [](Vec3 p) {
        return std::array<i64, 3>{std::llround(p.x * 256.0f), std::llround(p.y * 256.0f),
                                  std::llround(p.z * 256.0f)};
    };
    std::vector<Triangle> result;
    result.reserve(indices.size() / 3);
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        Triangle triangle{key(positions[indices[t]]), key(positions[indices[t + 1]]),
                          key(positions[indices[t + 2]])};
        const auto first = std::ranges::min_element(triangle) - triangle.begin();
        std::ranges::rotate(triangle, triangle.begin() + first);
        result.push_back(triangle);
    }
    std::ranges::sort(result);
    return result;
}

std::vector<Triangle> triangles(const std::map<ChunkCoord, Mesh>& meshes) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    for (const auto& [coord, mesh] : meshes) {
        const Vec3 origin{static_cast<f32>(chunk_origin(coord.x)),
                          static_cast<f32>(chunk_origin(coord.y)),
                          static_cast<f32>(chunk_origin(coord.z))};
        const auto base = static_cast<u32>(positions.size());
        for (const Vertex& v : mesh.vertices) {
            positions.push_back(origin + Vec3{v.px, v.py, v.pz});
        }
        for (const u32 index : mesh.indices) {
            indices.push_back(base + index);
        }
    }
    return triangles(positions, indices);
}

// Meshed alle vorgemerkten Chunks und trägt die Netze in meshes ein (leere entfernt).
void remesh(jobs::JobSystem& jobs, ChunkStore& store, std::map<ChunkCoord, Mesh>& meshes) {
    const std::vector<ChunkCoord> coords = store.take_changes();
    std::vector<Mesh> out(coords.size());
    mesh_chunks(jobs, store, coords, out);
    for (usize i = 0; i < coords.size(); ++i) {
        if (out[i].indices.empty()) {
            meshes.erase(coords[i]);
        } else {
            meshes[coords[i]] = std::move(out[i]);
        }
    }
}

// Dasselbe Feld als ein Gitter von -32 bis 32 Samples pro Achse, zum Vergleich.
Grid grid_of(const ChunkStore& store) {
    Grid grid{{65, 65, 65}};
    for (u32 z = 0; z < 65; ++z) {
        for (u32 y = 0; y < 65; ++y) {
            for (u32 x = 0; x < 65; ++x) {
                grid.set(x, y, z, store.sample(i64{x} - 32, i64{y} - 32, i64{z} - 32));
            }
        }
    }
    return grid;
}

} // namespace

TEST_CASE("Chunks hold one value until a sample differs", "[voxel]") {
    Chunk chunk;
    CHECK(chunk.uniform());
    CHECK(chunk.memory_bytes() == 0);
    CHECK(chunk.sample(5, 6, 7) == Sample{});
    // Derselbe Wert ändert nichts.
    chunk.set(1, 2, 3, Sample{});
    CHECK(chunk.uniform());

    chunk.set(1, 2, 3, {40, 2});
    CHECK_FALSE(chunk.uniform());
    CHECK(chunk.memory_bytes() >= chunk_sample_count * sizeof(Sample));
    CHECK(chunk.sample(1, 2, 3) == Sample{40, 2});
    CHECK(chunk.sample(0, 0, 0) == Sample{});
    CHECK_FALSE(chunk.compact());

    chunk.set(1, 2, 3, Sample{});
    CHECK(chunk.compact());
    CHECK(chunk.uniform());
    CHECK(chunk.memory_bytes() == 0);

    const Chunk solid{Sample{127, 3}};
    CHECK(solid.uniform_value() == Sample{127, 3});
}

TEST_CASE("Chunk coordinates round down for negative samples", "[voxel]") {
    CHECK(floor_div(0, 32) == 0);
    CHECK(floor_div(31, 32) == 0);
    CHECK(floor_div(32, 32) == 1);
    CHECK(floor_div(-1, 32) == -1);
    CHECK(floor_div(-32, 32) == -1);
    CHECK(floor_div(-33, 32) == -2);
    CHECK(chunk_of(-1, 0, 64) == ChunkCoord{-1, 0, 2, 0});
    CHECK(chunk_origin(-2) == -64);
    CHECK(chunk_position({1, -1, 2, 0}).x == 16.0);
    CHECK(chunk_position({1, -1, 2, 0}).y == -16.0);
    CHECK(chunk_position({1, 0, 0, 1}).x == 32.0);

    ChunkStore store;
    CHECK(store.sample(-5, 3, 100) == Sample{});
    CHECK(store.set(-5, 3, 100, {10, 1}));
    CHECK_FALSE(store.set(-5, 3, 100, {10, 1}));
    CHECK(store.sample(-5, 3, 100) == Sample{10, 1});
    REQUIRE(store.size() == 1);
    CHECK(store.coords().front() == ChunkCoord{-1, 0, 3, 0});
    // Luft in einen fehlenden Chunk zu schreiben legt keinen an.
    CHECK_FALSE(store.set(500, 0, 0, Sample{}));
    CHECK(store.size() == 1);
}

TEST_CASE("Changes mark the chunks whose border reads them", "[voxel]") {
    ChunkStore store;
    // Mitten im Chunk: nur er selbst.
    store.mark_changed(SampleBox{16, 16, 16, 17, 17, 17});
    CHECK(store.take_changes() == std::vector<ChunkCoord>{{0, 0, 0, 0}});
    // Am ersten Sample: Die Nachbarn davor lesen es als Randschicht.
    store.mark_changed(SampleBox{0, 16, 16, 1, 17, 17});
    CHECK(store.take_changes() == std::vector<ChunkCoord>{{-1, 0, 0, 0}, {0, 0, 0, 0}});
    // Am letzten Sample (31): Der Nachbar dahinter liest es für Normalen (zwei Samples davor).
    store.mark_changed(SampleBox{31, 16, 16, 32, 17, 17});
    CHECK(store.take_changes() == std::vector<ChunkCoord>{{0, 0, 0, 0}, {1, 0, 0, 0}});
    // An einer Ecke acht Chunks.
    store.mark_changed(SampleBox{0, 0, 0, 1, 1, 1});
    CHECK(store.take_changes().size() == 8);
    CHECK_FALSE(store.has_changes());
}

TEST_CASE("Gather copies a chunk with the border of its neighbours", "[voxel]") {
    jobs::JobSystem jobs{{.worker_count = 0}};
    ChunkStore store;
    // Ein Feld, das an jedem Sample anders ist, über 27 Chunks.
    fill_chunks(
        jobs, store, chunk_range(-1, 1),
        [](Vec3 p) {
            return std::sin(p.x * 0.31f) + std::cos(p.y * 0.17f) - std::sin(p.z * 0.23f);
        },
        [](Vec3 p) { return static_cast<Material>(1 + static_cast<i32>(std::abs(p.x)) % 3); });
    Grid padded{{padded_samples, padded_samples, padded_samples}};
    gather(store, {0, 0, 0, 0}, padded);
    bool equal = true;
    for (u32 z = 0; z < padded_samples; ++z) {
        for (u32 y = 0; y < padded_samples; ++y) {
            for (u32 x = 0; x < padded_samples; ++x) {
                equal = equal && padded.sample(x, y, z) == store.sample(i64{x} - border_before,
                                                                        i64{y} - border_before,
                                                                        i64{z} - border_before);
            }
        }
    }
    CHECK(equal);
    // Am Rand des Terrains liefern fehlende Nachbarn Luft.
    gather(store, {1, 1, 1, 0}, padded);
    CHECK(padded.sample(padded_samples - 1, 5, 5) == store.outside());
}

TEST_CASE("Empty and full chunks are stored as one value", "[voxel]") {
    jobs::JobSystem jobs;
    ChunkStore store;
    // Ebene bei y = 10: unten voll, oben leer, nur die Chunks bei y = 0 haben eine Oberfläche.
    fill_chunks(
        jobs, store, chunk_range(-1, 1), [](Vec3 p) { return 10.0f - p.y; },
        [](Vec3) { return Material{2}; });
    CHECK(store.size() == 27);
    CHECK(store.uniform_count() == 18);
    CHECK(store.memory_bytes() >= 9 * chunk_sample_count * sizeof(Sample));
    CHECK(store.memory_bytes() < 10 * chunk_sample_count * sizeof(Sample));
    // Nachbar bei y = 0 hat eine Oberfläche.
    CHECK_FALSE(trivially_empty(store, {0, -1, 0, 0}));
    // Nur fehlende Chunks: alles Luft.
    CHECK(trivially_empty(store, {0, 5, 0, 0}));
    // Volle Chunks neben fehlenden (Luft): dort liegt die Unterseite des Terrains.
    CHECK_FALSE(trivially_empty(store, {0, -2, 0, 0}));
}

TEST_CASE("A sphere over eight chunks is meshed without seams", "[voxel]") {
    jobs::JobSystem jobs;
    ChunkStore store;
    const Vec3 center{0.0f, 0.0f, 0.0f};
    // Mittelpunkt auf der Ecke von acht Chunks: Jede Chunk-Grenze schneidet die Kugel.
    fill_chunks(
        jobs, store, chunk_range(-1, 0), [&](Vec3 p) { return sphere(p, center, 20.0f); },
        [](Vec3) { return Material{2}; });
    std::map<ChunkCoord, Mesh> meshes;
    remesh(jobs, store, meshes);
    CHECK(meshes.size() == 8);

    const WorldMesh merged = merge(meshes);
    CHECK(closed(merged));
    // Jede Kante genau einmal: so viele Dreiecke wie das Netz desselben Felds aus einem Stück.
    Mesher mesher;
    Mesh reference;
    mesher.build(grid_of(store), reference);
    CHECK(merged.indices.size() == reference.indices.size());
    // Normalen zeigen nach außen, Dreiecke gegen den Uhrzeigersinn von außen.
    for (usize t = 0; t < merged.indices.size(); t += 3) {
        const Vec3 a = merged.positions[merged.indices[t]];
        const Vec3 b = merged.positions[merged.indices[t + 1]];
        const Vec3 c = merged.positions[merged.indices[t + 2]];
        REQUIRE(dot(cross(b - a, c - a), (a + b + c) * (1.0f / 3.0f) - center) > 0.0f);
    }
    for (const auto& [coord, mesh] : meshes) {
        for (const Vertex& v : mesh.vertices) {
            const Vec3 p = Vec3{static_cast<f32>(chunk_origin(coord.x)),
                                static_cast<f32>(chunk_origin(coord.y)),
                                static_cast<f32>(chunk_origin(coord.z))} +
                           Vec3{v.px, v.py, v.pz};
            REQUIRE(dot(Vec3{v.nx, v.ny, v.nz}, normalize(p - center)) > 0.95f);
        }
    }
}

TEST_CASE("Brushes and rays work across chunk borders", "[voxel]") {
    jobs::JobSystem jobs;
    ChunkStore store;
    fill_chunks(
        jobs, store, chunk_range(-1, 0), [](Vec3 p) { return sphere(p, {}, 20.0f); },
        [](Vec3) { return Material{2}; });
    std::map<ChunkCoord, Mesh> meshes;
    remesh(jobs, store, meshes);

    // Strahl entlang der Grenze zwischen vier Chunks von oben.
    const auto hit = raycast(store, {0.0f, 60.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    REQUIRE(hit);
    CHECK(std::abs(hit->position.y - 20.0f) < 0.2f);
    // Von der Seite durch die negativen Chunks.
    const auto side = raycast(store, {-60.0f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE(side);
    CHECK(std::abs(side->position.x + 20.0f) < 0.2f);
    CHECK_FALSE(raycast(store, {-60.0f, 30.0f, 0.5f}, {1.0f, 0.0f, 0.0f}, 100.0f));

    // Krater auf der Kuppe, mitten auf den Grenzen: nur die betroffenen Chunks neu meshen.
    const SampleBox box =
        apply(store, {.mode = BrushMode::Remove, .center = hit->position, .radius = 6.0f});
    REQUIRE_FALSE(box.empty());
    CHECK(store.sample(0, 19, 0).density <= 0);
    CHECK(store.sample(0, 19, 0).material == air);
    remesh(jobs, store, meshes);
    // Dieselben Dreiecke wie das Netz des ganzen Felds aus einem Gitter, also ohne Naht.
    Mesher mesher;
    Mesh reference;
    mesher.build(grid_of(store), reference);
    std::vector<Vec3> positions;
    for (const Vertex& v : reference.vertices) {
        positions.push_back(Vec3{v.px, v.py, v.pz} - Vec3{32.0f, 32.0f, 32.0f});
    }
    CHECK(triangles(meshes) == triangles(positions, reference.indices));

    // Auftragen außerhalb der Chunks legt neue an; Glätten ändert über Grenzen hinweg.
    const usize before = store.size();
    apply(store,
          {.mode = BrushMode::Add, .center = {0.0f, 40.0f, 0.0f}, .radius = 3.0f, .material = 3});
    CHECK(store.size() > before);
    CHECK(store.sample(0, 40, 0).material == 3);
    CHECK_FALSE(apply(store, {.mode = BrushMode::Smooth,
                              .center = {0.0f, 40.0f, 0.0f},
                              .radius = 4.0f,
                              .strength = 1.0f})
                    .empty());
    // Abtragen in leerer Luft ändert nichts und merkt nichts vor.
    static_cast<void>(store.take_changes());
    CHECK(apply(store, {.mode = BrushMode::Remove, .center = {200.0f, 200.0f, 200.0f}}).empty());
    CHECK_FALSE(store.has_changes());
}

TEST_CASE("Meshing in jobs gives the same meshes as on one thread", "[voxel][jobs]") {
    ChunkStore store;
    jobs::JobSystem serial{{.worker_count = 0}};
    jobs::JobSystem parallel{{.worker_count = 3}};
    fill_chunks(
        parallel, store, chunk_range(-2, 1),
        [](Vec3 p) { return 8.0f + 6.0f * std::sin(p.x * 0.1f) * std::cos(p.z * 0.13f) - p.y; },
        [](Vec3 p) { return p.y > 4.0f ? Material{1} : Material{3}; });
    const std::vector<ChunkCoord> coords = store.take_changes();
    REQUIRE(coords.size() > 64);
    std::vector<Mesh> a(coords.size());
    std::vector<Mesh> b(coords.size());
    mesh_chunks(serial, store, coords, a);
    mesh_chunks(parallel, store, coords, b);
    usize triangles = 0;
    for (usize i = 0; i < coords.size(); ++i) {
        REQUIRE(a[i].indices == b[i].indices);
        REQUIRE(a[i].vertices.size() == b[i].vertices.size());
        for (usize v = 0; v < a[i].vertices.size(); ++v) {
            REQUIRE(a[i].vertices[v].px == b[i].vertices[v].px);
            REQUIRE(a[i].vertices[v].weights_low == b[i].vertices[v].weights_low);
            REQUIRE(a[i].vertices[v].weights_high == b[i].vertices[v].weights_high);
        }
        triangles += a[i].triangle_count();
    }
    CHECK(triangles > 1000);
}
