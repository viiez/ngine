module;

#include <map>
#include <unordered_map>

export module ngine.render:terrain;

import ngine.core;
import ngine.rhi;
import ngine.world;
import ngine.voxel;
import :geometry;

export namespace ngine::render {

// Netz eines Chunks auf der GPU: Bereiche in den gemeinsamen Geometriepuffern.
struct TerrainChunkGpu {
    GeometryRange vertices;
    GeometryRange indices;
    // Fassung des Netzes (world::ChunkMesh::version), die hochgeladen ist.
    u64 version = 0;
    u64 vertex_address = 0;
    const rhi::Buffer* index_buffer = nullptr;
    u32 first_index = 0;
    u32 index_count = 0;
};

// Hält die Netze der Chunks aller Terrains in den Geometriepuffern des Renderers (R3.1): lädt neue
// Fassungen hoch, gibt veraltete und verschwundene frei. Der Renderer kennt keine Dichtefelder,
// nur fertige Netze aus world::TerrainData.
class TerrainMeshes {
public:
    explicit TerrainMeshes(GeometryBuffer& geometry) noexcept : geometry_{&geometry} {}
    ~TerrainMeshes();
    TerrainMeshes(const TerrainMeshes&) = delete;
    TerrainMeshes& operator=(const TerrainMeshes&) = delete;

    // Gleicht die Netze von data mit der GPU ab und liefert die Chunks mit Netz. uploads zählt
    // die hochgeladenen Netze. frame: laufende Nummer des Frames.
    const std::map<voxel::ChunkCoord, TerrainChunkGpu>& sync(const world::TerrainData& data,
                                                             u64 frame, u32& uploads);
    // Gibt Terrains frei, die seit zwei Frames niemand abgeglichen hat.
    void collect(u64 frame);

    [[nodiscard]] usize terrain_count() const noexcept {
        return terrains_.size();
    }

private:
    struct Entry {
        std::map<voxel::ChunkCoord, TerrainChunkGpu> chunks;
        u64 last_used = 0;
    };
    void release(TerrainChunkGpu& chunk);

    GeometryBuffer* geometry_;
    std::unordered_map<u64, Entry> terrains_;
};

} // namespace ngine::render
