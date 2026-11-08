module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

module ngine.world;

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.profile;
import ngine.reflect;
import ngine.voxel;

namespace ngine::world {

namespace {

using math::Vec3;

// Größte Ausdehnung eines Terrains in Chunks pro Achse; schützt vor Tippfehlern im Inspector.
constexpr i32 max_size = 64;

std::atomic<u64> next_terrain_id{1};
// Versionen der Netze über alle Terrains eindeutig: Ein Chunk, dessen Netz leer wird und später
// wiederkommt, bekommt nie eine Version, die der Renderer schon kennt.
std::atomic<u64> next_mesh_version{1};

f64 elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Höhe der Hügel in Voxeln über der Unterkante und ihr Gradient (in Voxeln pro Voxel). Einige
// Sinuswellen verschiedener Richtung und Länge; bis R3.2 der Generator aus Rauschen kommt.
f32 hills(const Terrain& terrain, f32 x, f32 z, f32& gx, f32& gz) {
    const auto meters = static_cast<f32>(voxel::voxel_size);
    const f32 ground = terrain.ground / meters;
    if (terrain.shape == TerrainShape::Flat) {
        gx = 0.0f;
        gz = 0.0f;
        return ground;
    }
    const f32 amplitude = terrain.height / meters;
    // Wellenlängen in Voxeln: rund 40 m, 26 m und 15 m.
    constexpr f32 k1 = 0.078f;
    constexpr f32 k2 = 0.12f;
    constexpr f32 k3 = 0.21f;
    const f32 a = std::sin(x * k1) * std::cos(z * k1 * 0.8f);
    const f32 b = std::sin((x + z) * k2 * 0.7f + 1.3f);
    const f32 c = std::cos(x * k3 * 0.6f - z * k3 * 0.9f + 0.4f);
    gx = amplitude * (0.55f * k1 * std::cos(x * k1) * std::cos(z * k1 * 0.8f) +
                      0.3f * k2 * 0.7f * std::cos((x + z) * k2 * 0.7f + 1.3f) -
                      0.15f * k3 * 0.6f * std::sin(x * k3 * 0.6f - z * k3 * 0.9f + 0.4f));
    gz = amplitude * (-0.55f * k1 * 0.8f * std::sin(x * k1) * std::sin(z * k1 * 0.8f) +
                      0.3f * k2 * 0.7f * std::cos((x + z) * k2 * 0.7f + 1.3f) +
                      0.15f * k3 * 0.9f * std::sin(x * k3 * 0.6f - z * k3 * 0.9f + 0.4f));
    return ground + amplitude * (0.55f * a + 0.3f * b + 0.15f * c);
}

Terrain clamped(Terrain terrain) {
    terrain.size_x = std::clamp(terrain.size_x, 1, max_size);
    terrain.size_y = std::clamp(terrain.size_y, 1, max_size);
    terrain.size_z = std::clamp(terrain.size_z, 1, max_size);
    return terrain;
}

} // namespace

voxel::ChunkCoord terrain_first_chunk(const Terrain& terrain) noexcept {
    const Terrain t = clamped(terrain);
    return {-t.size_x / 2, 0, -t.size_z / 2, 0};
}

void generate_terrain(const Terrain& terrain, voxel::ChunkStore& chunks, jobs::JobSystem& jobs) {
    NGINE_PROFILE_ZONE("world::generate_terrain");
    const Terrain t = clamped(terrain);
    const voxel::ChunkCoord first = terrain_first_chunk(t);
    chunks.clear();
    std::vector<voxel::ChunkCoord> coords;
    coords.reserve(static_cast<usize>(t.size_x) * static_cast<usize>(t.size_y) *
                   static_cast<usize>(t.size_z));
    for (i32 z = 0; z < t.size_z; ++z) {
        for (i32 y = 0; y < t.size_y; ++y) {
            for (i32 x = 0; x < t.size_x; ++x) {
                coords.push_back({first.x + x, first.y + y, first.z + z, 0});
            }
        }
    }
    // Abstand näherungsweise als Höhe über dem Punkt, geteilt durch die Länge des Gradienten der
    // Höhe; so bleibt er auch an Hängen nah am echten Abstand.
    const auto distance = [t](Vec3 p) {
        f32 gx = 0.0f;
        f32 gz = 0.0f;
        const f32 h = hills(t, p.x, p.z, gx, gz);
        return (h - p.y) / std::sqrt(1.0f + gx * gx + gz * gz);
    };
    // Gras an der Oberfläche, darunter Erde, tief unten Fels.
    const auto material = [distance](Vec3 p) -> voxel::Material {
        const f32 depth = distance(p);
        if (depth < 2.5f) {
            return 1;
        }
        return depth < 10.0f ? voxel::Material{3} : voxel::Material{2};
    };
    voxel::fill_chunks(jobs, chunks, coords, distance, material);
}

usize update_terrains(World& world, jobs::JobSystem& jobs) {
    NGINE_PROFILE_ZONE("world::update_terrains");
    // Neue Terrains bekommen ihren Zustand; das ist eine Strukturänderung, also nach der Abfrage.
    std::vector<Entity> missing;
    world.each<const Terrain>([&](Entity entity, const Terrain&) {
        if (!entity.has<TerrainState>()) {
            missing.push_back(entity);
        }
    });
    for (Entity entity : missing) {
        entity.set(TerrainState{});
    }

    usize meshed = 0;
    world.each<const Terrain, TerrainState>([&](const Terrain& terrain, TerrainState& state) {
        if (!state.data) {
            state.data = std::make_shared<TerrainData>();
            state.data->id = next_terrain_id.fetch_add(1, std::memory_order_relaxed);
        }
        TerrainData& data = *state.data;
        if (!data.generated || *data.generated != terrain) {
            const auto start = std::chrono::steady_clock::now();
            generate_terrain(terrain, data.chunks, jobs);
            data.generated = terrain;
            // Alte Netze weg; die neuen bekommen höhere Versionen, damit der Renderer sie ersetzt.
            for (auto& entry : data.meshes) {
                entry.second.mesh.clear();
            }
            data.stats.generate_ms = elapsed_ms(start);
        }
        if (!data.chunks.has_changes()) {
            data.stats.meshed = 0;
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        const std::vector<voxel::ChunkCoord> coords = data.chunks.take_changes();
        std::vector<voxel::Mesh> meshes(coords.size());
        voxel::mesh_chunks(jobs, data.chunks, coords, meshes);
        for (usize i = 0; i < coords.size(); ++i) {
            ChunkMesh& entry = data.meshes[coords[i]];
            entry.mesh = std::move(meshes[i]);
            entry.version = next_mesh_version.fetch_add(1, std::memory_order_relaxed);
        }
        // Chunks ohne Netz und ohne neue Fassung verschwinden aus der Liste.
        std::erase_if(data.meshes,
                      [](const auto& entry) { return entry.second.mesh.indices.empty(); });
        data.stats.meshed = static_cast<u32>(coords.size());
        data.stats.mesh_ms = elapsed_ms(start);
        data.stats.chunks = static_cast<u32>(data.chunks.size());
        data.stats.uniform_chunks = static_cast<u32>(data.chunks.uniform_count());
        data.stats.meshes = static_cast<u32>(data.meshes.size());
        data.stats.triangles = 0;
        for (const auto& entry : data.meshes) {
            data.stats.triangles += static_cast<u32>(entry.second.mesh.triangle_count());
        }
        data.stats.sample_bytes = data.chunks.memory_bytes();
        meshed += coords.size();
    });
    return meshed;
}

void register_terrain_types(reflect::TypeRegistry& types) {
    if (types.find<TerrainShape>() == nullptr) {
        types.add<TerrainShape>("TerrainShape")
            .value("Flat", TerrainShape::Flat)
            .value("Hills", TerrainShape::Hills);
    }
    if (types.find<Terrain>() == nullptr) {
        types.add<Terrain>("Terrain")
            .field("size_x", &Terrain::size_x)
            .range(1.0, max_size, 1.0)
            .field("size_y", &Terrain::size_y)
            .range(1.0, max_size, 1.0)
            .field("size_z", &Terrain::size_z)
            .range(1.0, max_size, 1.0)
            .field("shape", &Terrain::shape)
            .field("ground", &Terrain::ground)
            .range(0.0, 1024.0, 0.1)
            .field("height", &Terrain::height)
            .range(0.0, 512.0, 0.1);
    }
    if (types.find<TerrainState>() == nullptr) {
        // Ohne Felder: entsteht zur Laufzeit aus Terrain.
        types.add<TerrainState>("TerrainState").transient();
    }
}

} // namespace ngine::world
