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

// Größte Ausdehnung eines Terrains in Chunks pro Achse; schützt vor Tippfehlern im Inspector.
constexpr i32 max_size = 64;

std::atomic<u64> next_terrain_id{1};
// Versionen der Netze über alle Terrains eindeutig: Ein Chunk, dessen Netz leer wird und später
// wiederkommt, bekommt nie eine Version, die der Renderer schon kennt.
std::atomic<u64> next_mesh_version{1};

f64 elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

Terrain clamped(Terrain terrain) {
    terrain.size_x = std::clamp(terrain.size_x, 1, max_size);
    terrain.size_y = std::clamp(terrain.size_y, 1, max_size);
    terrain.size_z = std::clamp(terrain.size_z, 1, max_size);
    return terrain;
}

} // namespace

voxel::GeneratorDesc generator_desc(const Terrain& terrain) noexcept {
    voxel::GeneratorDesc desc{.seed = terrain.seed,
                              .ground = terrain.ground,
                              .hills = terrain.height,
                              .mountains = terrain.mountains,
                              .overhangs = terrain.overhangs,
                              .caves = std::clamp(terrain.caves, 0.0f, 1.0f),
                              .sand_level = terrain.sand_level};
    switch (terrain.shape) {
    case TerrainShape::Flat:
        desc.hills = 0.0f;
        desc.warp = 0.0f;
        [[fallthrough]];
    case TerrainShape::Hills:
        desc.mountains = 0.0f;
        desc.overhangs = 0.0f;
        desc.caves = 0.0f;
        break;
    case TerrainShape::Procedural: break;
    }
    return desc;
}

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
    voxel::generate_chunks(jobs, chunks, coords, voxel::Generator{generator_desc(t)});
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
            data.stats.generate_chunk_ms =
                data.stats.generate_ms / static_cast<f64>(std::max<usize>(data.chunks.size(), 1));
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
            .value("Hills", TerrainShape::Hills)
            .value("Procedural", TerrainShape::Procedural);
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
            .field("seed", &Terrain::seed)
            .field("ground", &Terrain::ground)
            .range(0.0, 1024.0, 0.1)
            .field("height", &Terrain::height)
            .range(0.0, 512.0, 0.1)
            .field("mountains", &Terrain::mountains)
            .range(0.0, 512.0, 0.1)
            .field("overhangs", &Terrain::overhangs)
            .range(0.0, 16.0, 0.05)
            .field("caves", &Terrain::caves)
            .range(0.0, 1.0, 0.01)
            .field("sand_level", &Terrain::sand_level)
            .range(-1024.0, 1024.0, 0.1);
    }
    if (types.find<TerrainState>() == nullptr) {
        // Ohne Felder: entsteht zur Laufzeit aus Terrain.
        types.add<TerrainState>("TerrainState").transient();
    }
}

} // namespace ngine::world
