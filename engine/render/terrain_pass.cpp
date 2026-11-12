module;

#include <array>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

// Terrain im Renderer (TD-36): Chunks der Terrains als Instanzen (R3.5) und ihre Materialtabelle
// mit Texturen aus dem Ressourcen-Manager (R3.2).
module ngine.render;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.asset;
import ngine.voxel;
import ngine.world;
import :renderer_detail;

namespace ngine::render {

using math::Vec3;
using math::Vec4;

std::vector<TerrainMaterial> default_terrain_materials() {
    // Farben linear; mit Textur Faktoren darauf (die Texturen bringen die eigentliche Farbe).
    return {
        {.name = "Luft"},
        {.name = "Gras", .color = {0.045f, 0.13f, 0.02f}, .roughness = 0.95f, .scale = 3.0f},
        {.name = "Fels", .color = {0.16f, 0.15f, 0.135f}, .roughness = 0.85f, .scale = 6.0f},
        {.name = "Erde", .color = {0.15f, 0.075f, 0.03f}, .roughness = 0.95f, .scale = 3.0f},
        {.name = "Sand", .color = {0.45f, 0.38f, 0.24f}, .roughness = 0.9f, .scale = 4.0f},
    };
}

void RendererCore::collect_terrain(world::World& world, math::DVec3 origin) {
    world.each<const world::WorldTransform, const world::TerrainState>(
        [&](world::Entity entity, const world::WorldTransform& transform,
            const world::TerrainState& state) {
            if (!state.data) {
                return;
            }
            const auto& chunks =
                terrain_meshes.sync(*state.data, frame_number, stats.terrain_uploads);
            // Chunks liegen achsenparallel an der Position der Entity (ADR 0038).
            const math::DVec3 base = transform.position();
            const auto id = static_cast<u32>(entity.id());
            for (const auto& [coord, chunk] : chunks) {
                if (!chunk.vertices.valid() || chunk.meshlet_count == 0) {
                    continue;
                }
                const auto scale =
                    static_cast<f32>(voxel::voxel_size * static_cast<f64>(1u << coord.level));
                const math::DVec3 local = voxel::chunk_position(coord);
                const Vec3 corner = math::to_f32(base + local - origin);
                // Hüllkugel des Chunks samt Randzellen.
                const f32 half = static_cast<f32>(voxel::chunk_voxels) * 0.5f * scale;
                GpuInstance instance{
                    .model = {Vec4{scale, 0.0f, 0.0f, corner.x}, Vec4{0.0f, scale, 0.0f, corner.y},
                              Vec4{0.0f, 0.0f, scale, corner.z}},
                    .sphere =
                        math::extend(corner + Vec3{half, half, half}, (half + scale) * 1.7321f),
                    .offset = math::extend(math::to_f32(local), scale),
                    .vertices = chunk.vertex_address,
                    .meshlets = chunk.meshlet_address,
                    .words = chunk.meshlet_address,
                    .meshlet_count = chunk.meshlet_count,
                    .id = id,
                    .palette = {chunk.palette[0], chunk.palette[1]},
                    .first_index = chunk.first_index,
                };
                instances.push_back(instance);
                instance_keys.push_back({.category = detail::Category::Terrain,
                                         .block = chunk.indices.block,
                                         .index_buffer = chunk.index_buffer});
                ++stats.terrain_chunks;
                ++stats.draws;
                stats.triangles += chunk.index_count / 3;
            }
        });
    terrain_meshes.collect(frame_number);
}

u64 RendererCore::write_terrain_materials(usize slot, bool any_terrain) {
    rhi::Buffer& buffer = *terrain_material_buffers[slot];
    if (!any_terrain) {
        return buffer.device_address();
    }
    // Index einer Textur, sobald sie geladen und hochgeladen ist; beginnt sonst das Laden.
    const auto resolve = [&]<typename T>(std::unordered_map<asset::AssetId, asset::Handle<T>>& map,
                                         asset::AssetId id) -> u32 {
        if (id == 0 || resources == nullptr) {
            return rhi::invalid_index;
        }
        auto [it, inserted] = map.try_emplace(id);
        if (inserted || !it->second) {
            it->second = resources->load<T>(id);
        }
        const T* texture = it->second.get();
        if (texture == nullptr || !device->upload_done(texture->ticket())) {
            return rhi::invalid_index;
        }
        return texture->index();
    };
    std::vector<GpuTerrainMaterial> table(terrain_materials.size());
    for (usize i = 0; i < terrain_materials.size(); ++i) {
        const TerrainMaterial& material = terrain_materials[i];
        table[i] = {.color = math::extend(material.color, material.roughness),
                    .params = {material.scale > 0.0f ? material.scale : 1.0f, material.normal_scale,
                               0.0f, 0.0f},
                    .textures = {resolve(color_textures, material.albedo),
                                 resolve(data_textures, material.normal), terrain_sampler, 0}};
    }
    if (!table.empty()) {
        buffer.write(0, std::as_bytes(std::span{table}));
    }
    return buffer.device_address();
}

} // namespace ngine::render
