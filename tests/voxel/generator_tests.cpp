#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ios>
#include <vector>

import ngine.core;
import ngine.jobs;
import ngine.voxel;

// Deterministisches Rauschen und Generator der Grundwelt (R3.2, ADR 0039).

using namespace ngine;

namespace {

std::vector<voxel::ChunkCoord> block(i32 size_x, i32 size_y, i32 size_z) {
    std::vector<voxel::ChunkCoord> coords;
    for (i32 z = -size_z / 2; z < size_z - size_z / 2; ++z) {
        for (i32 y = 0; y < size_y; ++y) {
            for (i32 x = -size_x / 2; x < size_x - size_x / 2; ++x) {
                coords.push_back({x, y, z, 0});
            }
        }
    }
    return coords;
}

// Wechsel zwischen fest und Luft entlang einer Säule von oben nach unten.
int transitions(const voxel::Generator& generator, i64 x, i64 z, i64 top) {
    int count = 0;
    bool solid = generator.sample(x, top, z).density > 0;
    for (i64 y = top - 1; y >= 0; --y) {
        const bool now = generator.sample(x, y, z).density > 0;
        count += now != solid ? 1 : 0;
        solid = now;
    }
    return count;
}

} // namespace

TEST_CASE("Noise is deterministic, bounded and depends on the seed", "[voxel]") {
    const voxel::Noise a{42};
    const voxel::Noise b{42};
    const voxel::Noise c{43};
    f64 min = 1.0;
    f64 max = -1.0;
    int different = 0;
    for (int i = 0; i < 2000; ++i) {
        const f64 x = static_cast<f64>(i) * 0.137 - 50.0;
        const f64 y = static_cast<f64>(i % 37) * 0.291 + 1e6;
        const f64 z = static_cast<f64>(i % 11) * -0.53;
        const f64 v2 = a.sample(x, y);
        const f64 v3 = a.sample(x, y, z);
        CHECK(v2 == b.sample(x, y));
        CHECK(v3 == b.sample(x, y, z));
        different += v3 != c.sample(x, y, z) ? 1 : 0;
        min = std::min({min, v2, v3});
        max = std::max({max, v2, v3});
    }
    CHECK(min >= -1.0);
    CHECK(max <= 1.0);
    // Das Rauschen nutzt den Wertebereich, ist also kein Rauschen um 0 mit winzigem Ausschlag.
    CHECK(min < -0.5);
    CHECK(max > 0.5);
    CHECK(different > 1900);
    // Auf den Gitterpunkten ist Simplex-Rauschen 0.
    CHECK(a.sample(0.0, 0.0) == 0.0);
    CHECK(a.sample(0.0, 0.0, 0.0) == 0.0);

    const f64 f = voxel::fbm(a, 3.3, -7.1, {.octaves = 5});
    CHECK(f >= -1.0);
    CHECK(f <= 1.0);
    const f64 r = voxel::ridged(a, 3.3, -7.1);
    CHECK(r >= 0.0);
    CHECK(r <= 1.0);
}

TEST_CASE("Generated chunks have a fixed checksum on every platform", "[voxel]") {
    // Bitgleich auf Linux (Clang) und Windows (MSVC): Beide CI-Läufe prüfen dieselbe Zahl. Ändert
    // sich der Generator absichtlich, ändert sich auch die Zahl; sonst ist das ein Fehler.
    const voxel::Generator generator{{.seed = 1234}};
    jobs::JobSystem jobs{{.worker_count = 2}};
    voxel::ChunkStore store;
    const auto coords = block(4, 3, 4);
    voxel::generate_chunks(jobs, store, coords, generator);
    const u64 sum = voxel::checksum(store);
    UNSCOPED_INFO("Pruefsumme: " << std::hex << sum);
    CHECK(sum == 0xfdbe419ab6ba3388ull);

    // Ohne Worker (alles in einem Thread) dieselbe Summe.
    jobs::JobSystem serial{{.worker_count = 0}};
    voxel::ChunkStore again;
    voxel::generate_chunks(serial, again, coords, generator);
    CHECK(voxel::checksum(again) == sum);
    // Ein anderer Startwert ergibt eine andere Welt.
    voxel::ChunkStore other;
    voxel::generate_chunks(serial, other, coords, voxel::Generator{{.seed = 1235}});
    CHECK(voxel::checksum(other) != sum);
}

TEST_CASE("Generator sample equals what fill writes", "[voxel]") {
    const voxel::Generator generator{{.seed = 7}};
    for (const voxel::ChunkCoord coord :
         {voxel::ChunkCoord{0, 0, 0, 0}, voxel::ChunkCoord{-3, 1, 2, 0},
          voxel::ChunkCoord{5, 0, -7, 0}}) {
        voxel::Chunk chunk;
        generator.fill(coord, chunk);
        const i64 ox = voxel::chunk_origin(coord.x);
        const i64 oy = voxel::chunk_origin(coord.y);
        const i64 oz = voxel::chunk_origin(coord.z);
        int mismatches = 0;
        for (u32 z = 0; z < 32; z += 3) {
            for (u32 y = 0; y < 32; ++y) {
                for (u32 x = 0; x < 32; x += 5) {
                    const voxel::Sample filled = chunk.sample(x, y, z);
                    const voxel::Sample single = generator.sample(ox + x, oy + y, oz + z);
                    mismatches += filled == single ? 0 : 1;
                }
            }
        }
        CHECK(mismatches == 0);
    }
}

TEST_CASE("Generator builds caves and overhangs only when asked", "[voxel]") {
    // Nur 2D-Regeln: eine Höhenkarte, jede Säule wechselt genau einmal von Luft zu fest.
    const voxel::Generator heightmap{
        {.seed = 3, .mountains = 0.0f, .overhangs = 0.0f, .caves = 0.0f}};
    for (i64 x = -200; x < 200; x += 23) {
        CHECK(transitions(heightmap, x, x / 2 + 7, 120) == 1);
    }

    // Mit allen Regeln gibt es Säulen mit mehreren Wechseln: Höhlen und Überhänge (VOX-09).
    const voxel::Generator world{{.seed = 3}};
    int multiple = 0;
    for (i64 x = -400; x < 400; x += 9) {
        multiple += transitions(world, x, -x / 3 + 11, 160) > 1 ? 1 : 0;
    }
    CHECK(multiple > 5);

    // Tief unter der Oberfläche liegen Hohlräume (Höhlen), ohne Höhlen nicht.
    const voxel::Generator no_caves{{.seed = 3, .caves = 0.0f}};
    int cave_samples = 0;
    int solid_samples = 0;
    for (i64 z = -300; z < 300; z += 5) {
        for (i64 x = -300; x < 300; x += 5) {
            const i64 y = 16;
            if (world.height(x, z) - static_cast<f64>(y) < 30.0) {
                continue;
            }
            cave_samples += world.sample(x, y, z).density <= 0 ? 1 : 0;
            solid_samples += no_caves.sample(x, y, z).density > 0 ? 1 : 0;
        }
    }
    CHECK(cave_samples > 20);
    CHECK(solid_samples > 1000);
}

TEST_CASE("Generator puts grass over earth over rock and sand in low places", "[voxel]") {
    // Flache Welt: Höhe genau ground.
    const voxel::Generator flat{{.seed = 1,
                                 .ground = 10.0f,
                                 .hills = 0.0f,
                                 .mountains = 0.0f,
                                 .warp = 0.0f,
                                 .overhangs = 0.0f,
                                 .caves = 0.0f,
                                 .sand_level = 5.0f}};
    CHECK(flat.height(123, -45) == 20.0);
    CHECK(flat.sample(0, 21, 0).material == voxel::air);
    CHECK(flat.sample(0, 21, 0).density < 0);
    CHECK(flat.sample(0, 20, 0).material == voxel::air); // genau auf der Oberfläche: Dichte 0
    CHECK(flat.sample(0, 19, 0).material == voxel::grass);
    CHECK(flat.sample(0, 17, 0).material == voxel::earth);
    CHECK(flat.sample(0, 10, 0).material == voxel::rock);

    const voxel::Generator beach{{.seed = 1,
                                  .ground = 4.0f,
                                  .hills = 0.0f,
                                  .mountains = 0.0f,
                                  .warp = 0.0f,
                                  .overhangs = 0.0f,
                                  .caves = 0.0f,
                                  .sand_level = 5.0f}};
    CHECK(beach.sample(3, 7, 3).material == voxel::sand);
    CHECK(beach.sample(3, 1, 3).material == voxel::rock);
}

TEST_CASE("Generator stores chunks far from the surface as one value", "[voxel]") {
    const voxel::Generator generator{{.seed = 5, .ground = 20.0f, .caves = 0.0f}};
    voxel::Chunk sky;
    generator.fill({0, 10, 0, 0}, sky);
    REQUIRE(sky.uniform());
    CHECK(sky.uniform_value() == voxel::Sample{-127, voxel::air});

    // 1 km tief: ohne Höhlen fester Fels.
    voxel::Chunk deep;
    generator.fill({0, -64, 0, 0}, deep);
    REQUIRE(deep.uniform());
    CHECK(deep.uniform_value() == voxel::Sample{127, voxel::rock});
    // Gleich dem einzelnen Sample.
    CHECK(generator.sample(5, 10 * 32 + 3, 7) == sky.uniform_value());
    CHECK(generator.sample(5, -64 * 32 + 3, 7) == deep.uniform_value());
}

TEST_CASE("Meshes of generated chunks blend materials over their vertices", "[voxel]") {
    const voxel::Generator generator{{.seed = 11}};
    jobs::JobSystem jobs{{.worker_count = 2}};
    voxel::ChunkStore store;
    const auto coords = block(4, 3, 4);
    voxel::generate_chunks(jobs, store, coords, generator);
    const std::vector<voxel::ChunkCoord> changed = store.take_changes();
    std::vector<voxel::Mesh> meshes(changed.size());
    voxel::mesh_chunks(jobs, store, changed, meshes);
    usize vertices = 0;
    usize blended = 0;
    bool sums = true;
    bool palettes = true;
    for (const voxel::Mesh& mesh : meshes) {
        palettes = palettes && mesh.palette_count <= voxel::palette_size;
        for (u32 slot = 0; slot < mesh.palette_count; ++slot) {
            palettes = palettes && mesh.palette[slot] != voxel::air;
        }
        for (const voxel::Vertex& v : mesh.vertices) {
            u32 sum = 0;
            u32 used = 0;
            for (u32 slot = 0; slot < voxel::palette_size; ++slot) {
                sum += v.weight(slot);
                used += v.weight(slot) >= 40 ? 1 : 0;
                // Gewichte nur auf belegten Plätzen.
                palettes = palettes && (slot < mesh.palette_count || v.weight(slot) == 0);
            }
            sums = sums && sum == 255;
            blended += used >= 2 ? 1 : 0;
            ++vertices;
        }
    }
    CHECK(sums);
    CHECK(palettes);
    REQUIRE(vertices > 1000);
    // An Grenzen zwischen Materialien (Gras und Fels, Erde und Fels) mischen sich die Gewichte.
    CHECK(blended > 50);
    CHECK(blended < vertices / 2);
}
