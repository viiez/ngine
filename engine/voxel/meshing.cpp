module;

#include <ngine/profile.hpp>
#include <span>
#include <vector>

module ngine.voxel;

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.profile;

namespace ngine::voxel {

void mesh_chunk(const ChunkStore& store, ChunkCoord coord, Mesher& mesher, Grid& scratch,
                Mesh& out) {
    NGINE_PROFILE_ZONE("voxel::mesh_chunk");
    if (trivially_empty(store, coord)) {
        out.clear();
        return;
    }
    gather(store, coord, scratch);
    mesher.build_chunk(scratch, out);
}

void mesh_chunks(jobs::JobSystem& jobs, const ChunkStore& store, std::span<const ChunkCoord> coords,
                 std::span<Mesh> out) {
    NGINE_PROFILE_ZONE("voxel::mesh_chunks");
    verify(out.size() == coords.size(), "voxel::mesh_chunks: out passt nicht zu coords");
    jobs.parallel_for(coords.size(), 1, [&](usize begin, usize end) {
        Mesher mesher;
        Grid scratch{{padded_samples, padded_samples, padded_samples}};
        for (usize i = begin; i < end; ++i) {
            mesh_chunk(store, coords[i], mesher, scratch, out[i]);
        }
    });
}

void fill_chunks(jobs::JobSystem& jobs, ChunkStore& store, std::span<const ChunkCoord> coords,
                 const DistanceFunction& distance, const MaterialFunction& material) {
    NGINE_PROFILE_ZONE("voxel::fill_chunks");
    // Anlegen vorab im aufrufenden Thread; danach ändert sich die Tabelle der Chunks nicht mehr.
    std::vector<Chunk*> chunks;
    chunks.reserve(coords.size());
    for (const ChunkCoord coord : coords) {
        verify(coord.level == 0, "voxel::fill_chunks: nur Stufe 0");
        chunks.push_back(&store.get_or_create(coord));
    }
    jobs.parallel_for(coords.size(), 1, [&](usize begin, usize end) {
        for (usize i = begin; i < end; ++i) {
            NGINE_PROFILE_ZONE("voxel::fill_chunk");
            const ChunkCoord coord = coords[i];
            const i64 ox = chunk_origin(coord.x);
            const i64 oy = chunk_origin(coord.y);
            const i64 oz = chunk_origin(coord.z);
            std::span<Sample> samples = chunks[i]->expand();
            for (u32 z = 0; z < chunk_voxels; ++z) {
                for (u32 y = 0; y < chunk_voxels; ++y) {
                    for (u32 x = 0; x < chunk_voxels; ++x) {
                        const math::Vec3 p{static_cast<f32>(ox + x), static_cast<f32>(oy + y),
                                           static_cast<f32>(oz + z)};
                        const i8 density = quantize(distance(p));
                        samples[Chunk::index(x, y, z)] = {density, density > 0 ? material(p) : air};
                    }
                }
            }
            chunks[i]->compact();
        }
    });
    for (const ChunkCoord coord : coords) {
        store.mark_changed(coord);
    }
}

} // namespace ngine::voxel
