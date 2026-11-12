module;

#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

export module ngine.render:terrain;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.asset;
import ngine.world;
import ngine.voxel;
import :geometry;

export namespace ngine::render {

// Ein Material des Terrains (VOX-07, R3.2, ADR 0040). Der Index in der Tabelle ist die
// Materialnummer der Voxel (voxel::Material, 0 ist Luft). Texturen kommen über ihre Asset-Kennung
// aus dem Ressourcen-Manager des Renderers und werden triplanar aufgetragen; ohne Textur (oder
// solange sie lädt) zeigt das Terrain nur die Farbe.
struct TerrainMaterial {
    std::string name;
    // Grundfarbe (linear); mit Textur ein Faktor darauf.
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    f32 roughness = 0.9f;
    // Meter pro Wiederholung der Texturen.
    f32 scale = 4.0f;
    f32 normal_scale = 1.0f;
    // Grundfarbe (sRGB) und Normalen im Tangentenraum (linear); 0 heißt keine.
    asset::AssetId albedo = 0;
    asset::AssetId normal = 0;
};

// Größte Zahl von Materialien in der Tabelle (Material ist 8 Bit).
inline constexpr u32 max_terrain_materials = 256;

// Material des Terrains im GPU-Puffer, wie terrain.slang es liest.
struct GpuTerrainMaterial {
    // xyz Farbe, w Roughness.
    math::Vec4 color;
    // x Meter pro Wiederholung, y Stärke der Normalen.
    math::Vec4 params;
    // Grundfarbe, Normalen, Sampler (bindlose Indizes), frei.
    u32 textures[4] = {};
};
static_assert(sizeof(GpuTerrainMaterial) == 48);

// Standardtabelle ohne Texturen: 1 Gras, 2 Fels, 3 Erde, 4 Sand (wie voxel::grass usw.).
[[nodiscard]] std::vector<TerrainMaterial> default_terrain_materials();

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
    // Materialpalette des Netzes (voxel::Mesh::palette), vier Materialnummern je Wort.
    std::array<u32, 2> palette{};
};

// Hält die Netze der Chunks aller Terrains in den Geometriepuffern des Renderers (R3.1): lädt neue
// Fassungen hoch, gibt veraltete und verschwundene frei. Der Renderer kennt keine Dichtefelder,
// nur fertige Netze aus world::TerrainData.
//
// Seit R3.3 mit Budget pro Frame (TD-39): Neue Fassungen landen zuerst in eigenen Bereichen, höchstens
// upload_budget Bytes pro Frame (mindestens ein Netz). Gezeichnet werden sie erst, wenn alle Netze
// eines Terrains so weit sind; dann tauscht sync() alle auf einmal. So bleibt die Übernahme des
// Streamings ohne Risse, auch wenn ihre Netze über mehrere Frames hochgeladen werden.
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

    // Bytes, die sync() pro Frame über alle Terrains höchstens hochlädt.
    u64 upload_budget = u64{8} << 20U;

    [[nodiscard]] usize terrain_count() const noexcept {
        return terrains_.size();
    }

private:
    struct Entry {
        // Gezeichnete Netze.
        std::map<voxel::ChunkCoord, TerrainChunkGpu> chunks;
        // Hochgeladene, noch nicht gezeichnete Fassungen (ohne Bereiche: Netz ist leer).
        std::map<voxel::ChunkCoord, TerrainChunkGpu> staged;
        u64 last_used = 0;
    };
    void release(TerrainChunkGpu& chunk);
    // Lädt mesh nach chunk hoch; false, wenn der Platz fehlt.
    bool upload(const world::ChunkMesh& mesh, TerrainChunkGpu& chunk);

    GeometryBuffer* geometry_;
    std::unordered_map<u64, Entry> terrains_;
    // Budget des laufenden Frames.
    u64 budget_frame_ = ~u64{0};
    u64 budget_left_ = 0;
    bool uploaded_in_frame_ = false;
};

} // namespace ngine::render
