module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

module ngine.world;

import ngine.core;
import ngine.filesystem;
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
    return {
        .lod = {.min_chunk_y = 0,
                .max_chunk_y = t.size_y,
                .view_distance = static_cast<f64>(std::clamp(t.view_distance, 16.0f, 16000.0f)) /
                                 voxel::voxel_size,
                .detail = static_cast<f64>(std::clamp(t.detail, 1.0f, 8.0f))}};
}

voxel::ChunkCoord terrain_first_chunk(const Terrain& terrain) noexcept {
    const Terrain t = clamped(terrain);
    return {-t.size_x / 2, 0, -t.size_z / 2, 0};
}

void generate_terrain(const Terrain& terrain, voxel::ChunkStore& chunks, jobs::JobSystem& jobs,
                      const voxel::DeltaMap* deltas) {
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
    if (deltas != nullptr) {
        // Nur wenige Chunks haben ein Delta; außerhalb des Terrains gelten keine.
        for (const auto& [coord, delta] : *deltas) {
            if (voxel::Chunk* chunk = chunks.find(coord)) {
                delta->apply(*chunk);
            }
        }
    }
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
    data.stats.delta_chunks = static_cast<u32>(data.deltas.size());
    data.stats.delta_bytes = data.deltas.encoded_bytes();
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
    // Alte Netze gibt ein Job frei: Hunderte große Blöcke auf einmal kosten im Hauptthread
    // spürbar Zeit (vor allem unter Windows).
    std::vector<voxel::Mesh> old;
    const auto retire = [&](voxel::ChunkCoord coord) {
        if (const auto it = data.meshes.find(coord); it != data.meshes.end()) {
            old.push_back(std::move(it->second.mesh));
            data.meshes.erase(it);
        }
    };
    for (const voxel::ChunkCoord coord : changes.removed) {
        retire(coord);
    }
    for (auto& [coord, mesh] : changes.meshes) {
        if (mesh.indices.empty()) {
            retire(coord);
            continue;
        }
        ChunkMesh& entry = data.meshes[coord];
        old.push_back(std::move(entry.mesh));
        entry.mesh = std::move(mesh);
        entry.version = next_mesh_version.fetch_add(1, std::memory_order_relaxed);
    }
    if (!old.empty()) {
        jobs.run([retired = std::move(old)] { static_cast<void>(retired); });
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
            // Die Deltas bleiben, auch wenn sich die Grundwelt ändert (R3.4).
            data.deltas.set_generator(generator_desc(terrain));
            if (terrain.streaming) {
                data.chunks.clear();
                data.meshes.clear();
                data.streamer = std::make_unique<voxel::Streamer>(generator_desc(terrain),
                                                                  stream_desc(terrain));
                data.streamer->set_deltas(&data.deltas);
            } else {
                generate_terrain(terrain, data.chunks, jobs, data.deltas.snapshot().get());
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

// --- Bearbeiten (R3.4) --------------------------------------------------------------------------

namespace {

TerrainData* terrain_data(Entity terrain) {
    if (!terrain || !terrain.alive()) {
        return nullptr;
    }
    TerrainState* state = terrain.has<TerrainState>() ? terrain.get_mut<TerrainState>() : nullptr;
    return state != nullptr ? state->data.get() : nullptr;
}

math::DVec3 terrain_base(Entity terrain) {
    return terrain.has<WorldTransform>() ? terrain.get<WorldTransform>()->position()
                                         : math::DVec3{};
}

math::Vec3 to_vec3(math::DVec3 v) {
    return {static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
}

void count_deltas(TerrainData& data) {
    data.stats.delta_chunks = static_cast<u32>(data.deltas.size());
    data.stats.delta_bytes = data.deltas.encoded_bytes();
}

} // namespace

math::DVec3 terrain_voxels(Entity terrain, math::DVec3 world_position) {
    return (world_position - terrain_base(terrain)) * (1.0 / voxel::voxel_size);
}

math::DVec3 terrain_world(Entity terrain, math::DVec3 voxels) {
    return terrain_base(terrain) + voxels * voxel::voxel_size;
}

std::optional<math::DVec3> raycast_terrain(Entity terrain, math::DVec3 origin, math::Vec3 direction,
                                           f64 max_distance) {
    const TerrainData* data = terrain_data(terrain);
    if (data == nullptr) {
        return std::nullopt;
    }
    // In Voxeln relativ zum Terrain; f32 reicht dort, weil das Terrain nah an der Kamera liegt.
    const math::Vec3 local = to_vec3(terrain_voxels(terrain, origin));
    const auto length = static_cast<f32>(max_distance / voxel::voxel_size);
    const std::optional<voxel::RayHit> hit =
        data->streamer
            ? voxel::raycast(data->streamer->layout(), data->chunks, local, direction, length)
            : voxel::raycast(data->chunks, local, direction, length);
    if (!hit) {
        return std::nullopt;
    }
    return terrain_world(terrain, math::DVec3{hit->position.x, hit->position.y, hit->position.z});
}

voxel::TerrainEdit edit_terrain(Entity terrain, const voxel::Brush& brush) {
    NGINE_PROFILE_ZONE("world::edit_terrain");
    TerrainData* data = terrain_data(terrain);
    if (data == nullptr || !data->generated) {
        return {};
    }
    // Ein Terrain fester Größe hat außerhalb seiner Chunks nichts.
    voxel::TerrainEdit edit = data->deltas.apply(data->chunks, brush, !data->streamer);
    if (data->streamer) {
        data->streamer->invalidate(edit.box);
    }
    count_deltas(*data);
    return edit;
}

void restore_terrain(Entity terrain, std::span<const voxel::DeltaChange> changes, bool after) {
    TerrainData* data = terrain_data(terrain);
    if (data == nullptr) {
        return;
    }
    const voxel::SampleBox box = data->deltas.restore(data->chunks, changes, after);
    if (data->streamer) {
        data->streamer->invalidate(box);
    }
    count_deltas(*data);
}

std::shared_ptr<const voxel::DeltaMap> terrain_deltas(Entity terrain) {
    const TerrainData* data = terrain_data(terrain);
    return data != nullptr ? data->deltas.snapshot() : nullptr;
}

void set_terrain_deltas(Entity terrain, std::shared_ptr<const voxel::DeltaMap> deltas) {
    if (!terrain || !terrain.alive()) {
        return;
    }
    TerrainData* data = terrain_data(terrain);
    if (data == nullptr) {
        auto created = std::make_shared<TerrainData>();
        created->id = next_terrain_id.fetch_add(1, std::memory_order_relaxed);
        data = created.get();
        terrain.set(TerrainState{std::move(created)});
    }
    data->deltas.assign(std::move(deltas));
    // Alles neu aus Grundwelt und Deltas, beim nächsten update_terrains().
    data->generated.reset();
    count_deltas(*data);
}

// --- Speichern (R3.4) ---------------------------------------------------------------------------

namespace {

// Terrains der Welt mit ihren Laufzeitdaten, über die Reflection, damit es auch mit einer
// konstanten Welt geht.
struct TerrainEntry {
    u64 entity = 0;
    const Terrain* terrain = nullptr;
    const TerrainData* data = nullptr;
};

std::vector<TerrainEntry> list_terrains(const World& world) {
    const reflect::TypeInfo* terrain_type = world.types().find<Terrain>();
    const reflect::TypeInfo* state_type = world.types().find<TerrainState>();
    std::vector<TerrainEntry> result;
    if (terrain_type == nullptr) {
        return result;
    }
    for (const u64 entity : world.entity_ids()) {
        TerrainEntry entry{.entity = entity};
        auto visit = [&](const reflect::TypeInfo& type, const void* data) {
            if (&type == terrain_type) {
                entry.terrain = static_cast<const Terrain*>(data);
            } else if (&type == state_type) {
                entry.data = static_cast<const TerrainState*>(data)->data.get();
            }
        };
        world.for_each_component(
            entity, ComponentVisitor{.context = &visit,
                                     .visit = [](void* context, const reflect::TypeInfo& type,
                                                 ComponentId /*component*/, const void* data) {
                                         (*static_cast<decltype(visit)*>(context))(type, data);
                                     }});
        if (entry.terrain != nullptr) {
            result.push_back(entry);
        }
    }
    return result;
}

std::string delta_folder(std::string_view scene_path, const Terrain& terrain) {
    return filesystem::join(filesystem::parent_path(scene_path), terrain.deltas);
}

// Name der Szene ohne Ordner und Endung.
std::string scene_stem(std::string_view scene_path) {
    std::string name = filesystem::file_name(scene_path);
    if (const usize dot = name.rfind('.'); dot != std::string::npos && dot > 0) {
        name.resize(dot);
    }
    return name.empty() ? std::string{"szene"} : name;
}

} // namespace

usize assign_terrain_folders(World& world, std::string_view scene_path) {
    std::unordered_set<std::string> used;
    std::vector<u64> missing;
    for (const TerrainEntry& entry : list_terrains(world)) {
        if (!entry.terrain->deltas.empty()) {
            used.insert(entry.terrain->deltas);
        } else if (entry.data != nullptr && entry.data->deltas.size() > 0) {
            missing.push_back(entry.entity);
        }
    }
    const std::string stem = scene_stem(scene_path);
    u32 suffix = 1;
    for (const u64 id : missing) {
        std::string name;
        do {
            name = suffix == 1 ? stem + ".terrain" : std::format("{}.terrain{}", stem, suffix);
            ++suffix;
        } while (used.contains(name));
        used.insert(name);
        Entity entity = world.entity(id);
        entity.get_mut<Terrain>()->deltas = name;
        entity.modified<Terrain>();
        // Die Beschreibung ändert sich nur im Namen des Ordners; das Terrain bleibt, wie es ist.
        if (TerrainData* data = terrain_data(entity); data != nullptr && data->generated) {
            data->generated->deltas = name;
        }
    }
    return missing.size();
}

Result<void> save_terrain_deltas(const World& world, std::string_view scene_path) {
    NGINE_PROFILE_ZONE("world::save_terrain_deltas");
    for (const TerrainEntry& entry : list_terrains(world)) {
        const usize count = entry.data != nullptr ? entry.data->deltas.size() : 0;
        if (entry.terrain->deltas.empty()) {
            if (count > 0) {
                log::warn("Terrain ohne Ordner für Deltas: {} geänderte Chunks nicht gespeichert",
                          count);
            }
            continue;
        }
        if (entry.data == nullptr) {
            // Nie geladen, also auch nicht geändert: Was im Ordner liegt, bleibt.
            continue;
        }
        const std::string folder = delta_folder(scene_path, *entry.terrain);
        if (count == 0 && !filesystem::is_directory(folder)) {
            continue;
        }
        if (auto created = filesystem::create_directories(folder); !created) {
            return std::unexpected(std::move(created.error()));
        }
        std::unordered_set<std::string> written;
        usize bytes = 0;
        for (const auto& [region, data] : voxel::encode_regions(*entry.data->deltas.snapshot())) {
            std::string name = voxel::region_file_name(region);
            if (auto result = filesystem::write_file(filesystem::join(folder, name), data);
                !result) {
                return std::unexpected(std::move(result.error()));
            }
            bytes += data.size();
            written.insert(std::move(name));
        }
        // Regionen, in denen es keine Deltas mehr gibt, verschwinden.
        if (auto listed = filesystem::list_directory(folder)) {
            for (const filesystem::FileEntry& file : *listed) {
                if (!file.directory && voxel::parse_region_file_name(file.path) &&
                    !written.contains(file.path)) {
                    static_cast<void>(filesystem::remove(filesystem::join(folder, file.path)));
                }
            }
        }
        log::info("Deltas des Terrains gespeichert: {} Chunks in {} Regionen, {:.1f} KiB ({})",
                  count, written.size(), static_cast<f64>(bytes) / 1024.0, folder);
    }
    return {};
}

Result<usize> load_terrain_deltas(World& world, std::string_view scene_path) {
    NGINE_PROFILE_ZONE("world::load_terrain_deltas");
    usize total = 0;
    for (const TerrainEntry& entry : list_terrains(world)) {
        if (entry.data != nullptr || entry.terrain->deltas.empty()) {
            continue;
        }
        const std::string folder = delta_folder(scene_path, *entry.terrain);
        auto deltas = std::make_shared<voxel::DeltaMap>();
        if (filesystem::is_directory(folder)) {
            auto listed = filesystem::list_directory(folder);
            if (!listed) {
                return std::unexpected(std::move(listed.error()));
            }
            for (const filesystem::FileEntry& file : *listed) {
                const auto region = voxel::parse_region_file_name(file.path);
                if (file.directory || !region) {
                    continue;
                }
                const std::string path = filesystem::join(folder, file.path);
                auto bytes = filesystem::read_file(path);
                if (!bytes) {
                    return std::unexpected(std::move(bytes.error()));
                }
                if (auto decoded = voxel::decode_region(*region, *bytes, *deltas); !decoded) {
                    return std::unexpected(std::format("{}: {}", path, decoded.error()));
                }
            }
        }
        total += deltas->size();
        set_terrain_deltas(world.entity(entry.entity), std::move(deltas));
    }
    return total;
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
            .range(1.0, 8.0, 0.05)
            .field("deltas", &Terrain::deltas);
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
