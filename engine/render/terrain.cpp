module;

#include <array>
#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <unordered_map>

module ngine.render;

import ngine.core;
import ngine.rhi;
import ngine.voxel;
import ngine.world;

namespace ngine::render {

TerrainMeshes::~TerrainMeshes() {
    for (auto& [id, entry] : terrains_) {
        for (auto& [coord, chunk] : entry.chunks) {
            release(chunk);
        }
    }
}

void TerrainMeshes::release(TerrainChunkGpu& chunk) {
    geometry_->free(chunk.vertices);
    geometry_->free(chunk.indices);
    chunk = {};
}

const std::map<voxel::ChunkCoord, TerrainChunkGpu>&
TerrainMeshes::sync(const world::TerrainData& data, u64 frame, u32& uploads) {
    Entry& entry = terrains_[data.id];
    entry.last_used = frame;
    // Chunks, die kein Netz mehr haben.
    for (auto it = entry.chunks.begin(); it != entry.chunks.end();) {
        if (!data.meshes.contains(it->first)) {
            release(it->second);
            it = entry.chunks.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [coord, mesh] : data.meshes) {
        TerrainChunkGpu& chunk = entry.chunks[coord];
        if (chunk.version == mesh.version && chunk.vertices.valid()) {
            continue;
        }
        release(chunk);
        chunk.version = mesh.version;
        if (mesh.mesh.indices.empty()) {
            continue;
        }
        const auto vertices = std::as_bytes(std::span{mesh.mesh.vertices});
        const auto indices = std::as_bytes(std::span{mesh.mesh.indices});
        auto vertex_range = geometry_->allocate(vertices.size());
        if (!vertex_range) {
            log::error("Terrain: {}", vertex_range.error());
            continue;
        }
        auto index_range = geometry_->allocate(indices.size());
        if (!index_range) {
            log::error("Terrain: {}", index_range.error());
            geometry_->free(*vertex_range);
            continue;
        }
        // Änderungen sollen im selben Frame zu sehen sein: Kopie vor der nächsten Abgabe.
        geometry_->upload(*vertex_range, vertices);
        geometry_->upload(*index_range, indices);
        chunk.vertices = *vertex_range;
        chunk.indices = *index_range;
        chunk.vertex_address = geometry_->address(*vertex_range);
        chunk.index_buffer = &geometry_->buffer(*index_range);
        chunk.first_index = static_cast<u32>(index_range->offset / sizeof(u32));
        chunk.index_count = static_cast<u32>(mesh.mesh.indices.size());
        chunk.palette = {};
        for (u32 slot = 0; slot < mesh.mesh.palette_count; ++slot) {
            chunk.palette[slot / 4] |= u32{mesh.mesh.palette[slot]} << ((slot % 4) * 8);
        }
        ++uploads;
    }
    return entry.chunks;
}

void TerrainMeshes::collect(u64 frame) {
    std::erase_if(terrains_, [&](auto& item) {
        if (item.second.last_used + 2 >= frame) {
            return false;
        }
        for (auto& [coord, chunk] : item.second.chunks) {
            release(chunk);
        }
        return true;
    });
}

} // namespace ngine::render
