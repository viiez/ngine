module;

// Jolt nur in den Dateien jolt*.cpp und jolt.cppm (ADR 0045). Jolt.h zuerst: Es setzt die Schalter,
// die alle anderen Header brauchen.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Kollision des Terrains (R4.3): eine Form pro Chunk der Stufe 0 aus dem Netz, das der Mesher
// ohnehin baut, in Jobs; Körper ohne Boden schlafen eingefroren, bis er da ist.
module ngine.physics;

import ngine.core;
import ngine.math;
import ngine.jobs;
import ngine.profile;
import ngine.world;
import :jolt;

namespace ngine::physics {

void PhysicsWorld::Impl::remove_chunk_body(TerrainChunk& chunk) {
    if (chunk.body.IsInvalid()) {
        return;
    }
    terrain_of_body.erase(chunk.body.GetIndexAndSequenceNumber());
    body_interface().RemoveBody(chunk.body);
    body_interface().DestroyBody(chunk.body);
    chunk.body = {};
}

// Lage aller Dinge, die Formen brauchen: dynamische Körper und Beobachter (Spielfiguren
// folgen mit ihnen).
std::vector<math::DVec3> PhysicsWorld::Impl::terrain_anchors() {
    std::vector<math::DVec3> anchors;
    JPH::BodyInterface& bodies_interface = body_interface();
    for (const auto& [entity, record] : bodies) {
        if (record.body.motion == MotionType::Dynamic) {
            anchors.push_back(from_jolt(bodies_interface.GetPosition(record.id)));
        }
    }
    for (const auto& [entity, record] : characters) {
        anchors.push_back(from_jolt(record.character->GetPosition()));
    }
    world->each<const world::TerrainObserver, const world::WorldTransform>(
        [&](world::Entity, const world::TerrainObserver& observer,
            const world::WorldTransform& transform) {
            if (observer.active) {
                anchors.push_back(transform.position());
            }
        });
    return anchors;
}

// Chunks der Stufe 0 eines Terrains, die eine Form brauchen: mit Netz und im Radius um einen
// Anker. Sortiert.
std::vector<voxel::ChunkCoord>
PhysicsWorld::Impl::needed_chunks(const world::TerrainData& data, math::DVec3 base,
                                  std::span<const math::DVec3> anchors) const {
    const f64 radius = static_cast<f64>(std::max(settings.terrain_radius, 0.0f));
    const auto reach = static_cast<i32>(std::ceil(radius / voxel::chunk_size));
    std::unordered_set<voxel::ChunkCoord, voxel::ChunkCoordHash> centers;
    for (const math::DVec3& anchor : anchors) {
        const math::DVec3 local = (anchor - base) * (1.0 / voxel::voxel_size);
        centers.insert(voxel::chunk_of(static_cast<i64>(std::floor(local.x)),
                                       static_cast<i64>(std::floor(local.y)),
                                       static_cast<i64>(std::floor(local.z))));
    }
    std::unordered_set<voxel::ChunkCoord, voxel::ChunkCoordHash> needed;
    for (const voxel::ChunkCoord center : centers) {
        for (i32 dz = -reach; dz <= reach; ++dz) {
            for (i32 dy = -reach; dy <= reach; ++dy) {
                for (i32 dx = -reach; dx <= reach; ++dx) {
                    const voxel::ChunkCoord coord{center.x + dx, center.y + dy, center.z + dz, 0};
                    if (needed.contains(coord) || !data.meshes.contains(coord)) {
                        continue;
                    }
                    needed.insert(coord);
                }
            }
        }
    }
    // Genauer: nur Chunks, deren Würfel näher als radius an einem Anker liegt. Die Mitten der
    // Würfel reichen als Maß; reach deckt die Nachbarn ab.
    std::vector<voxel::ChunkCoord> result;
    result.reserve(needed.size());
    const f64 half = voxel::chunk_size * 0.5;
    const f64 limit = radius + half * 1.7320508;
    for (const voxel::ChunkCoord coord : needed) {
        const math::DVec3 center =
            base + voxel::chunk_position(coord) + math::DVec3{half, half, half};
        for (const math::DVec3& anchor : anchors) {
            const math::DVec3 d = center - anchor;
            if (d.x * d.x + d.y * d.y + d.z * d.z <= limit * limit) {
                result.push_back(coord);
                break;
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Startet einen Job, der die Form aus dem Netz baut.
void PhysicsWorld::Impl::start_build(TerrainChunk& chunk, const world::ChunkMesh& mesh) {
    auto build = std::make_shared<TerrainBuild>();
    build->version = mesh.version;
    JPH::VertexList vertices;
    vertices.reserve(mesh.mesh.vertices.size());
    const auto scale = static_cast<f32>(voxel::voxel_size);
    for (const voxel::Vertex& v : mesh.mesh.vertices) {
        vertices.push_back(JPH::Float3{v.px * scale, v.py * scale, v.pz * scale});
    }
    // Material eines Dreiecks: der Platz der Palette mit dem größten Gewicht an seinen drei
    // Ecken (höchstens acht Plätze, Jolt erlaubt 32 Materialien pro Form).
    const voxel::Mesh& source = mesh.mesh;
    const u32 slots = std::max(source.palette_count, 1u);
    JPH::PhysicsMaterialList materials;
    for (u32 slot = 0; slot < slots; ++slot) {
        materials.push_back(JPH::RefConst<JPH::PhysicsMaterial>{surfaces[source.palette[slot]]});
    }
    JPH::IndexedTriangleList triangles;
    triangles.reserve(source.indices.size() / 3);
    for (usize i = 0; i + 2 < source.indices.size(); i += 3) {
        const u32 a = source.indices[i];
        const u32 b = source.indices[i + 1];
        const u32 c = source.indices[i + 2];
        u32 best = 0;
        u32 best_weight = 0;
        for (u32 slot = 0; slot < source.palette_count; ++slot) {
            const u32 weight = source.vertices[a].weight(slot) + source.vertices[b].weight(slot) +
                               source.vertices[c].weight(slot);
            if (weight > best_weight) {
                best = slot;
                best_weight = weight;
            }
        }
        triangles.push_back(JPH::IndexedTriangle{a, b, c, best});
    }
    build->vertices = std::move(vertices);
    build->input = std::move(triangles);
    build->materials = std::move(materials);
    chunk.build = build;
    auto work = [build] {
        const auto start = std::chrono::steady_clock::now();
        build->triangles = static_cast<u32>(build->input.size());
        build->shape = terrain_shape(std::move(build->vertices), std::move(build->input),
                                     std::move(build->materials));
        build->ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                        .count();
        build->done.store(true, std::memory_order_release);
    };
    jobs->run(std::move(work), &terrain_jobs);
}

// Setzt eine fertige Form ein; liefert, ob sich die Kollision geändert hat.
bool PhysicsWorld::Impl::finish_build(u64 terrain_entity, const TerrainRecord& terrain,
                                      voxel::ChunkCoord coord, TerrainChunk& chunk, f64& build_ms,
                                      u32& built) {
    std::shared_ptr<TerrainBuild> build = std::move(chunk.build);
    chunk.build.reset();
    built += 1;
    build_ms += build->ms;
    JPH::BodyInterface& bodies_interface = body_interface();
    if (build->shape == nullptr) {
        // Netz ohne Dreiecke: keine Form.
        remove_chunk_body(chunk);
        chunk.version = build->version;
        chunk.triangles = 0;
        chunk.bytes = 0;
        return true;
    }
    chunk.version = build->version;
    chunk.triangles = build->triangles;
    chunk.bytes = build->shape->GetStats().mSizeBytes;
    if (chunk.body.IsInvalid()) {
        JPH::BodyCreationSettings creation{
            build->shape.GetPtr(), to_jolt(terrain.base + voxel::chunk_position(coord)),
            JPH::Quat::sIdentity(), JPH::EMotionType::Static, object_layer(PhysicsLayer::Static)};
        creation.mUserData = terrain_entity;
        JPH::Body* body = bodies_interface.CreateBody(creation);
        if (body == nullptr) {
            warn_once(terrain_entity, "kein Platz mehr für Formen des Terrains");
            return false;
        }
        chunk.body = body->GetID();
        bodies_interface.AddBody(chunk.body, JPH::EActivation::DontActivate);
        terrain_of_body[chunk.body.GetIndexAndSequenceNumber()] = terrain_entity;
    } else {
        bodies_interface.SetShape(chunk.body, build->shape.GetPtr(), false,
                                  JPH::EActivation::DontActivate);
    }
    // Körper in der Nähe wecken: Liegt eine Kiste auf weggegrabenem Boden, fällt sie.
    const math::DVec3 corner = terrain.base + voxel::chunk_position(coord);
    constexpr f64 margin = 1.0;
    const auto low = math::to_f32(corner - math::DVec3{margin, margin, margin});
    const auto high =
        math::to_f32(corner + math::DVec3{voxel::chunk_size + margin, voxel::chunk_size + margin,
                                          voxel::chunk_size + margin});
    const QueryBroadFilter broad{all_layers};
    const QueryObjectFilter object{all_layers};
    bodies_interface.ActivateBodiesInAABox(
        JPH::AABox{JPH::Vec3{low.x, low.y, low.z}, JPH::Vec3{high.x, high.y, high.z}}, broad,
        object);
    return true;
}

// Hält die Formen aller Terrains aktuell.
void PhysicsWorld::Impl::sync_terrains() {
    NGINE_PROFILE_ZONE("PhysicsWorld::sync_terrains");
    struct Live {
        u64 entity;
        std::shared_ptr<world::TerrainData> data;
        math::DVec3 base;
    };
    std::vector<Live> live;
    world->each<const world::TerrainState, const world::WorldTransform>(
        [&](world::Entity entity, const world::TerrainState& state,
            const world::WorldTransform& transform) {
            if (state.data) {
                live.push_back(Live{entity.id(), state.data, transform.position()});
            }
        });
    std::ranges::sort(live, {}, &Live::entity);

    // Weggefallene Terrains.
    for (auto it = terrains.begin(); it != terrains.end();) {
        const bool alive =
            std::ranges::any_of(live, [&](const Live& l) { return l.entity == it->first; });
        if (!alive) {
            for (auto& [coord, chunk] : it->second.chunks) {
                remove_chunk_body(chunk);
            }
            it = terrains.erase(it);
        } else {
            ++it;
        }
    }
    if (live.empty()) {
        stats.terrain_chunks = 0;
        stats.terrain_building = 0;
        stats.terrain_built = 0;
        stats.terrain_bytes = 0;
        return;
    }

    const std::vector<math::DVec3> anchors = terrain_anchors();
    f64 build_ms = 0.0;
    u32 built = 0;
    // Ohne wait_for_rebuilds wartet der Schritt nur auf Chunks ohne Form; ändert sich ein Netz mit
    // Form (Graben), bleibt die alte, bis die neue fertig ist (R5.4).
    bool missing = false;
    for (const Live& l : live) {
        TerrainRecord& terrain = terrains[l.entity];
        if (terrain.data_id != l.data->id) {
            for (auto& [coord, chunk] : terrain.chunks) {
                remove_chunk_body(chunk);
            }
            terrain.chunks.clear();
            terrain.data_id = l.data->id;
            terrain.base = l.base;
        }
        if (!(terrain.base == l.base)) {
            // Das Terrain wurde verschoben: alle Formen mit.
            terrain.base = l.base;
            for (auto& [coord, chunk] : terrain.chunks) {
                if (!chunk.body.IsInvalid()) {
                    body_interface().SetPosition(chunk.body,
                                                 to_jolt(l.base + voxel::chunk_position(coord)),
                                                 JPH::EActivation::DontActivate);
                }
            }
        }
        const std::vector<voxel::ChunkCoord> needed = needed_chunks(*l.data, l.base, anchors);
        for (const voxel::ChunkCoord coord : needed) {
            TerrainChunk& chunk = terrain.chunks[coord];
            chunk.unused = 0;
            const world::ChunkMesh& mesh = l.data->meshes.at(coord);
            const bool current = chunk.version == mesh.version;
            const bool building = chunk.build && chunk.build->version == mesh.version;
            if (!current && !building) {
                start_build(chunk, mesh);
            }
            if (!current && chunk.body.IsInvalid()) {
                missing = true;
            }
        }
        // Formen ohne Bedarf oder ohne Netz fallen nach einer Weile weg.
        for (auto it = terrain.chunks.begin(); it != terrain.chunks.end();) {
            TerrainChunk& chunk = it->second;
            const bool has_mesh = l.data->meshes.contains(it->first);
            if (!std::binary_search(needed.begin(), needed.end(), it->first)) {
                ++chunk.unused;
            }
            if (!has_mesh || chunk.unused > settings.terrain_keep_steps) {
                if (!chunk.build) {
                    remove_chunk_body(chunk);
                    it = terrain.chunks.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    if (settings.wait_for_terrain && (missing || settings.wait_for_rebuilds)) {
        jobs->wait(terrain_jobs);
    }
    // Fertige Formen einsetzen, in der Reihenfolge der Terrains und Chunks.
    u32 building = 0;
    u32 chunks_with_shape = 0;
    usize bytes = 0;
    for (auto& [entity, terrain] : terrains) {
        for (auto it = terrain.chunks.begin(); it != terrain.chunks.end();) {
            TerrainChunk& chunk = it->second;
            if (chunk.build && chunk.build->done.load(std::memory_order_acquire)) {
                (void)finish_build(entity, terrain, it->first, chunk, build_ms, built);
                if (chunk.body.IsInvalid() && chunk.unused > settings.terrain_keep_steps) {
                    it = terrain.chunks.erase(it);
                    continue;
                }
            }
            if (chunk.build) {
                ++building;
            }
            if (!chunk.body.IsInvalid()) {
                ++chunks_with_shape;
                bytes += chunk.bytes;
            }
            ++it;
        }
    }
    stats.terrain_chunks = chunks_with_shape;
    stats.terrain_building = building;
    stats.terrain_built = built;
    if (built > 0) {
        stats.terrain_build_ms = build_ms / static_cast<f64>(built);
    }
    stats.terrain_bytes = bytes;
}

// Ob unter einer Lage die Kollision des Terrains fehlt: Der Chunk der Stufe 0 an der Lage
// oder einer der beiden darunter hat ein Netz, aber noch keine Form dazu, oder ein endloses
// Terrain hat dort noch gar keine Chunks der Stufe 0 geladen.
bool PhysicsWorld::Impl::missing_ground(math::DVec3 position) const {
    for (const auto& [entity, terrain] : terrains) {
        const world::Entity handle = world->entity(entity);
        const auto* state =
            static_cast<const world::TerrainState*>(world->get(entity, terrain_state_id));
        if (state == nullptr || !state->data || !handle) {
            continue;
        }
        const world::TerrainData& data = *state->data;
        const math::DVec3 local = (position - terrain.base) * (1.0 / voxel::voxel_size);
        const voxel::ChunkCoord at = voxel::chunk_of(static_cast<i64>(std::floor(local.x)),
                                                     static_cast<i64>(std::floor(local.y)),
                                                     static_cast<i64>(std::floor(local.z)));
        bool loaded = false;
        for (i32 dy = 0; dy <= 2; ++dy) {
            const voxel::ChunkCoord coord{at.x, at.y - dy, at.z, 0};
            if (data.chunks.find(coord) != nullptr) {
                loaded = true;
            }
            const auto mesh = data.meshes.find(coord);
            if (mesh == data.meshes.end()) {
                continue;
            }
            loaded = true;
            const auto chunk = terrain.chunks.find(coord);
            if (chunk == terrain.chunks.end() || chunk->second.version == 0) {
                return true;
            }
        }
        if (!loaded && data.streamer) {
            return true;
        }
    }
    return false;
}

// Friert dynamische Körper ohne Boden ein und taut sie auf, sobald er da ist.
void PhysicsWorld::Impl::update_frozen() {
    NGINE_PROFILE_ZONE("PhysicsWorld::update_frozen");
    JPH::BodyInterface& bodies_interface = body_interface();
    for (auto& [entity, record] : bodies) {
        if (record.body.motion != MotionType::Dynamic) {
            continue;
        }
        const bool is_frozen = frozen.contains(entity);
        if (!is_frozen && !bodies_interface.IsActive(record.id)) {
            // Schlafende Körper bleiben liegen, bis jemand sie weckt.
            continue;
        }
        const bool missing =
            !terrains.empty() && missing_ground(from_jolt(bodies_interface.GetPosition(record.id)));
        if (missing && !is_frozen) {
            bodies_interface.SetMotionType(record.id, JPH::EMotionType::Kinematic,
                                           JPH::EActivation::DontActivate);
            bodies_interface.SetLinearAndAngularVelocity(record.id, JPH::Vec3::sZero(),
                                                         JPH::Vec3::sZero());
            frozen[entity] = true;
        } else if (!missing && is_frozen) {
            bodies_interface.SetMotionType(record.id, JPH::EMotionType::Dynamic,
                                           JPH::EActivation::Activate);
            bodies_interface.SetLinearAndAngularVelocity(record.id,
                                                         to_jolt(record.body.linear_velocity),
                                                         to_jolt(record.body.angular_velocity));
            frozen.erase(entity);
        }
    }
    stats.frozen_bodies = static_cast<u32>(frozen.size());
}
} // namespace ngine::physics
