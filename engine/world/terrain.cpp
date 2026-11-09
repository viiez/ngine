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

voxel::StreamDesc stream_desc(const Terrain& terrain) noexcept {
    const Terrain t = clamped(terrain);
    return {.lod = {.min_chunk_y = 0,
                    .max_chunk_y = t.size_y,
                    .view_distance =
                        static_cast<f64>(std::clamp(t.view_distance, 16.0f, 16000.0f)) /
                        voxel::voxel_size,
                    .detail = static_cast<f64>(std::clamp(t.detail, 1.0f, 8.0f))}};
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

namespace {

void count_meshes(TerrainData& data) {
    data.stats.chunks = static_cast<u32>(data.chunks.size());
    data.stats.uniform_chunks = static_cast<u32>(data.chunks.uniform_count());
    data.stats.meshes = static_cast<u32>(data.meshes.size());
    data.stats.triangles = 0;
    data.stats.mesh_bytes = 0;
    for (const auto& entry : data.meshes) {
        const voxel::Mesh& mesh = entry.second.mesh;
        data.stats.triangles += static_cast<u32>(mesh.triangle_count());
        data.stats.mesh_bytes +=
            mesh.vertices.size() * sizeof(voxel::Vertex) + mesh.indices.size() * sizeof(u32);
    }
    data.stats.sample_bytes = data.chunks.memory_bytes();
}

// Ein Schritt des Streamings eines endlosen Terrains; Beobachter in Voxeln relativ zu seinem
// Ursprung. Liefert die Zahl der neuen Netze.
usize stream_terrain(TerrainData& data, jobs::JobSystem& jobs,
                     std::span<const math::DVec3> observers, bool wait) {
    const auto start = std::chrono::steady_clock::now();
    voxel::StreamChanges changes = data.streamer->update(jobs, data.chunks, observers, wait);
    data.stats.streaming = true;
    data.stats.stream = data.streamer->stats();
    data.stats.meshed = static_cast<u32>(changes.meshes.size());
    if (changes.empty()) {
        return 0;
    }
    for (const voxel::ChunkCoord coord : changes.removed) {
        data.meshes.erase(coord);
    }
    for (auto& [coord, mesh] : changes.meshes) {
        if (mesh.indices.empty()) {
            data.meshes.erase(coord);
            continue;
        }
        ChunkMesh& entry = data.meshes[coord];
        entry.mesh = std::move(mesh);
        entry.version = next_mesh_version.fetch_add(1, std::memory_order_relaxed);
    }
    data.stats.mesh_ms = elapsed_ms(start);
    count_meshes(data);
    return changes.meshes.size();
}

} // namespace

usize update_terrains(World& world, jobs::JobSystem& jobs, const TerrainUpdate& update) {
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

    // Beobachter in Weltkoordinaten (R3.3).
    std::vector<math::DVec3> observers{update.observers.begin(), update.observers.end()};
    world.each<const TerrainObserver, const WorldTransform>(
        [&](const TerrainObserver& observer, const WorldTransform& transform) {
            if (observer.active) {
                observers.push_back(transform.position());
            }
        });

    usize meshed = 0;
    world.each<const Terrain, TerrainState>([&](Entity entity, const Terrain& terrain,
                                                TerrainState& state) {
        if (!state.data) {
            state.data = std::make_shared<TerrainData>();
            state.data->id = next_terrain_id.fetch_add(1, std::memory_order_relaxed);
        }
        TerrainData& data = *state.data;
        if (!data.generated || *data.generated != terrain) {
            const auto start = std::chrono::steady_clock::now();
            // Alte Netze weg; die neuen bekommen höhere Versionen, damit der Renderer sie ersetzt.
            for (auto& entry : data.meshes) {
                entry.second.mesh.clear();
            }
            data.streamer.reset();
            data.stats = {};
            if (terrain.streaming) {
                data.chunks.clear();
                data.meshes.clear();
                data.streamer =
                    std::make_unique<voxel::Streamer>(generator_desc(terrain), stream_desc(terrain));
            } else {
                generate_terrain(terrain, data.chunks, jobs);
                data.stats.generate_ms = elapsed_ms(start);
                data.stats.generate_chunk_ms =
                    data.stats.generate_ms /
                    static_cast<f64>(std::max<usize>(data.chunks.size(), 1));
            }
            data.generated = terrain;
        }
        if (data.streamer) {
            // Beobachter relativ zum Ursprung des Terrains in Voxeln; ohne Beobachter die Mitte
            // der Oberfläche über der Position des Terrains.
            const math::DVec3 base = entity.has<WorldTransform>()
                                         ? entity.get<WorldTransform>()->position()
                                         : math::DVec3{};
            std::vector<math::DVec3> local;
            local.reserve(std::max<usize>(observers.size(), 1));
            for (const math::DVec3& p : observers) {
                local.push_back((p - base) * (1.0 / voxel::voxel_size));
            }
            if (local.empty()) {
                local.push_back({0.0, static_cast<f64>(terrain.ground) / voxel::voxel_size, 0.0});
            }
            meshed += stream_terrain(data, jobs, local, update.wait);
            return;
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
        count_meshes(data);
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
            .range(-1024.0, 1024.0, 0.1)
            .field("streaming", &Terrain::streaming)
            .field("view_distance", &Terrain::view_distance)
            .range(16.0, 16000.0, 1.0)
            .field("detail", &Terrain::detail)
            .range(1.0, 8.0, 0.05);
    }
    if (types.find<TerrainObserver>() == nullptr) {
        types.add<TerrainObserver>("TerrainObserver").field("active", &TerrainObserver::active);
    }
    if (types.find<TerrainState>() == nullptr) {
        // Ohne Felder: entsteht zur Laufzeit aus Terrain.
        types.add<TerrainState>("TerrainState").transient();
    }
}

} // namespace ngine::world
