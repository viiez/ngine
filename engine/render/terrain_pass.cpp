module;

#include <array>
#include <span>
#include <unordered_map>
#include <vector>

// Terrain im Renderer (TD-36): Chunks der Terrains als Draws, ihre Materialtabelle mit Texturen
// aus dem Ressourcen-Manager (R3.2) und das Zeichnen der Chunks in allen Passes.
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

void Renderer::Impl::collect_terrain(world::World& world, math::DVec3 origin, bool shadows_on,
                                     const std::array<Cascade, cascade_count>& cascades) {
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
                if (!chunk.vertices.valid()) {
                    continue;
                }
                const auto scale =
                    static_cast<f32>(voxel::voxel_size * static_cast<f64>(1u << coord.level));
                const math::DVec3 local = voxel::chunk_position(coord);
                const Vec3 corner = math::to_f32(base + local - origin);
                detail::TerrainDraw draw{.model = {Vec4{scale, 0.0f, 0.0f, corner.x},
                                                   Vec4{0.0f, scale, 0.0f, corner.y},
                                                   Vec4{0.0f, 0.0f, scale, corner.z}},
                                         .offset = math::extend(math::to_f32(local), scale),
                                         .vertices = chunk.vertex_address,
                                         .index_buffer = chunk.index_buffer,
                                         .first_index = chunk.first_index,
                                         .index_count = chunk.index_count,
                                         .id = id,
                                         .palette = chunk.palette};
                const auto index = static_cast<u32>(terrain_draws.size());
                terrain_draws.push_back(draw);
                if (shadows_on) {
                    // Hüllkugel des Chunks samt Randzellen.
                    const f32 half = static_cast<f32>(voxel::chunk_voxels) * 0.5f * scale;
                    const Vec3 center = corner + Vec3{half, half, half};
                    const f32 radius = (half + scale) * 1.7321f;
                    for (u32 i = 0; i < cascade_count; ++i) {
                        if (cascades[i].may_cast(center, radius)) {
                            terrain_shadow_draws[i].push_back(index);
                        }
                    }
                }
            }
        });
    terrain_meshes.collect(frame_number);
    stats.terrain_chunks = static_cast<u32>(terrain_draws.size());
}

u64 Renderer::Impl::write_terrain_materials(usize slot) {
    rhi::Buffer& buffer = *terrain_material_buffers[slot];
    if (terrain_draws.empty()) {
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

void Renderer::Impl::draw_terrain(rhi::CommandList& commands,
                                  std::span<const detail::TerrainDraw> draws, u64 frame_address,
                                  u64 materials_address, u32 cascade) {
    const rhi::Buffer* indices = nullptr;
    for (const detail::TerrainDraw& draw : draws) {
        if (indices != draw.index_buffer) {
            indices = draw.index_buffer;
            commands.bind_index_buffer(*indices);
        }
        const TerrainPush push{.model = {draw.model[0], draw.model[1], draw.model[2]},
                               .offset = draw.offset,
                               .vertices = draw.vertices,
                               .frame = frame_address,
                               .materials = materials_address,
                               .cascade = cascade,
                               .id = draw.id,
                               .palette = {draw.palette[0], draw.palette[1]}};
        commands.push_constants(std::as_bytes(std::span{&push, 1}));
        commands.draw_indexed(draw.index_count, 1, draw.first_index);
    }
}

} // namespace ngine::render
