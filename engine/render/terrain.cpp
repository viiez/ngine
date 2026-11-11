module;

#include <algorithm>
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
        for (auto& [coord, chunk] : entry.staged) {
            release(chunk);
        }
    }
}

void TerrainMeshes::release(TerrainChunkGpu& chunk) {
    geometry_->free(chunk.vertices);
    geometry_->free(chunk.indices);
    chunk = {};
}

bool TerrainMeshes::upload(const world::ChunkMesh& mesh, TerrainChunkGpu& chunk) {
    chunk.version = mesh.version;
    if (mesh.mesh.indices.empty()) {
        return true;
    }
    const auto vertices = std::as_bytes(std::span{mesh.mesh.vertices});
    const auto indices = std::as_bytes(std::span{mesh.mesh.indices});
    auto vertex_range = geometry_->allocate(vertices.size());
    if (!vertex_range) {
        log::error("Terrain: {}", vertex_range.error());
        return false;
    }
    auto index_range = geometry_->allocate(indices.size());
    if (!index_range) {
        log::error("Terrain: {}", index_range.error());
        geometry_->free(*vertex_range);
        return false;
    }
    // Kopie vor der nächsten Abgabe; gezeichnet wird erst nach dem Tausch in sync().
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
    return true;
}

const std::map<voxel::ChunkCoord, TerrainChunkGpu>&
TerrainMeshes::sync(const world::TerrainData& data, u64 frame, u32& uploads) {
    if (budget_frame_ != frame) {
        budget_frame_ = frame;
        budget_left_ = upload_budget;
        uploaded_in_frame_ = false;
    }
    Entry& entry = terrains_[data.id];
    entry.last_used = frame;
    // Fassungen, die es nicht mehr gibt, fallen aus dem Zwischenstand.
    for (auto it = entry.staged.begin(); it != entry.staged.end();) {
        const auto mesh = data.meshes.find(it->first);
        if (mesh == data.meshes.end() || mesh->second.version != it->second.version) {
            release(it->second);
            it = entry.staged.erase(it);
        } else {
            ++it;
        }
    }
    // Neue Fassungen hochladen, solange das Budget reicht; mindestens ein Netz pro Frame, damit es
    // immer weitergeht.
    usize missing = 0;
    for (const auto& [coord, mesh] : data.meshes) {
        const auto shown = entry.chunks.find(coord);
        if (shown != entry.chunks.end() && shown->second.version == mesh.version) {
            continue;
        }
        if (entry.staged.contains(coord)) {
            continue;
        }
        const u64 bytes = mesh.mesh.vertices.size() * sizeof(voxel::Vertex) +
                          mesh.mesh.indices.size() * sizeof(u32);
        if (bytes > budget_left_ && uploaded_in_frame_) {
            ++missing;
            continue;
        }
        TerrainChunkGpu staged;
        if (!upload(mesh, staged)) {
            // Kein Platz: ohne Netz weiter, statt den Tausch für immer aufzuhalten.
            entry.staged.emplace(coord, staged);
            continue;
        }
        budget_left_ -= std::min(bytes, budget_left_);
        uploaded_in_frame_ = true;
        entry.staged.emplace(coord, staged);
        ++uploads;
    }
    if (missing > 0) {
        return entry.chunks;
    }
    // Alles da: in einem Schritt tauschen, verschwundene Chunks freigeben.
    for (auto& [coord, staged] : entry.staged) {
        TerrainChunkGpu& shown = entry.chunks[coord];
        release(shown);
        shown = staged;
    }
    entry.staged.clear();
    for (auto it = entry.chunks.begin(); it != entry.chunks.end();) {
        if (!data.meshes.contains(it->first) || !it->second.vertices.valid()) {
            release(it->second);
            it = entry.chunks.erase(it);
        } else {
            ++it;
        }
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
        for (auto& [coord, chunk] : item.second.staged) {
            release(chunk);
        }
        return true;
    });
}

} // namespace ngine::render
