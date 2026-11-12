module;

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module ngine.voxel:lod;

import ngine.core;
import ngine.math;
import ngine.jobs;
import :grid;
import :chunk;
import :mesh;
import :edit;

export namespace ngine::voxel {

// Größte Stufe im Baum der Chunks. Ein Chunk der Stufe 10 ist 32 768 Voxel (16 km) breit.
inline constexpr u8 max_lod_level = 10;

// Breite eines Chunks der Stufe level in Voxeln.
[[nodiscard]] constexpr i64 chunk_extent(u8 level) noexcept {
    return i64{chunk_voxels} << level;
}

// Achsenparalleler Bereich eines Chunks in Voxeln (Stufe 0), min inklusive, max exklusive.
[[nodiscard]] constexpr SampleBox chunk_box(ChunkCoord c) noexcept {
    const i64 e = chunk_extent(c.level);
    return {i64{c.x} * e,     i64{c.y} * e,     i64{c.z} * e,
            i64{c.x} * e + e, i64{c.y} * e + e, i64{c.z} * e + e};
}

// Welche Chunks der Baum um die Beobachter auswählt (VOX-04, WORLD-07, R3.3, ADR 0041). Alle
// Längen in Voxeln relativ zum Ursprung des Terrains.
struct LodDesc {
    // Senkrechter Bereich in Chunks der Stufe 0, [min_chunk_y, max_chunk_y): Chunks, die ihn nicht
    // schneiden, gibt es nicht.
    i32 min_chunk_y = 0;
    i32 max_chunk_y = 4;
    // Chunks weiter weg als das von jedem Beobachter gibt es nicht.
    f64 view_distance = 4000.0;
    // Ein Chunk teilt sich in acht, solange ein Beobachter näher als detail × seine Breite ist.
    // Größer heißt feiner und mehr Chunks.
    f64 detail = 2.0;
    // Stufe der Wurzeln; 0 heißt: aus view_distance und detail.
    u8 max_level = 0;
};

// Stufe der Wurzeln für eine Beschreibung: so grob, dass sich jede Wurzel im Sichtbereich teilt.
[[nodiscard]] u8 root_level(const LodDesc& desc) noexcept;

// Die Blätter des Baums: Chunks verschiedener Stufen, die sich nicht überlappen. Ausgeglichen
// heißt: Benachbarte Blätter (Fläche, Kante oder Ecke) unterscheiden sich höchstens um eine Stufe.
// Darauf verlassen sich die Nähte des Meshers.
class LodLayout {
public:
    LodLayout() = default;
    explicit LodLayout(std::vector<ChunkCoord> leaves);

    [[nodiscard]] const std::vector<ChunkCoord>& leaves() const noexcept {
        return leaves_;
    }
    [[nodiscard]] usize size() const noexcept {
        return leaves_.size();
    }
    [[nodiscard]] bool contains(ChunkCoord coord) const noexcept {
        return set_.contains(coord);
    }
    // Blatt, das den Punkt enthält; Koordinaten verdoppelt (2 × Voxel), damit Punkte zwischen
    // Samples ganzzahlig sind. Ungerade Koordinaten liegen nie auf einer Grenze.
    [[nodiscard]] std::optional<ChunkCoord> leaf_at2(i64 x2, i64 y2, i64 z2) const noexcept;
    // Blatt, dem das Sample gehört (Bereiche sind unten inklusive, oben exklusiv).
    [[nodiscard]] std::optional<ChunkCoord> leaf_of(i64 x, i64 y, i64 z) const noexcept {
        return leaf_at2(2 * x + 1, 2 * y + 1, 2 * z + 1);
    }
    // Blätter, die coord berühren (Fläche, Kante, Ecke), mit coord selbst, sortiert. Bei einem
    // ausgeglichenen Baum genau die Blätter, deren Daten das Netz von coord liest.
    [[nodiscard]] std::vector<ChunkCoord> neighbourhood(ChunkCoord coord) const;
    // Ob benachbarte Blätter sich höchstens um eine Stufe unterscheiden.
    [[nodiscard]] bool balanced() const;
    // Anzahl der Blätter pro Stufe.
    [[nodiscard]] std::array<u32, max_lod_level + 1> level_counts() const noexcept;

    friend bool operator==(const LodLayout& a, const LodLayout& b) noexcept {
        return a.leaves_ == b.leaves_;
    }

private:
    std::vector<ChunkCoord> leaves_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> set_;
    u8 max_level_ = 0;
    u32 level_mask_ = 0;
};

// Wählt die Blätter um die Beobachter (Positionen in Voxeln relativ zum Ursprung des Terrains):
// Nahes fein, Fernes grob, ausgeglichen (VOX-04, WORLD-07).
[[nodiscard]] LodLayout select_lod(std::span<const math::DVec3> observers, const LodDesc& desc);

// Kleinster Abstand eines Beobachters zum Bereich des Chunks (0 innerhalb), in Voxeln.
[[nodiscard]] f64 observer_distance(std::span<const math::DVec3> observers,
                                    ChunkCoord coord) noexcept;

// Woher der LodMesher die Samples der Blätter liest: Chunk zu einer Koordinate (nullptr: fehlt,
// gilt als outside()) und der Wert außerhalb. find() muss aus mehreren Jobs gleichzeitig gehen.
class ChunkSource {
public:
    ChunkSource() = default;
    ChunkSource(const ChunkSource&) = default;
    ChunkSource& operator=(const ChunkSource&) = default;
    virtual ~ChunkSource() = default;
    [[nodiscard]] virtual const Chunk* find(ChunkCoord coord) const noexcept = 0;
    [[nodiscard]] virtual Sample outside() const noexcept = 0;
};

// Feste Zuordnung von Koordinaten zu Chunks, die anderswo liegen. Damit meshen Jobs, während der
// ChunkStore im Hauptthread weiter geändert wird (Streamer, R3.3).
class ChunkMap final : public ChunkSource {
public:
    explicit ChunkMap(Sample outside = {}) noexcept : outside_{outside} {}
    void add(ChunkCoord coord, const Chunk* chunk) {
        chunks_.insert_or_assign(coord, chunk);
    }
    void clear() noexcept {
        chunks_.clear();
    }
    void set_outside(Sample outside) noexcept {
        outside_ = outside;
    }
    [[nodiscard]] usize size() const noexcept {
        return chunks_.size();
    }
    [[nodiscard]] const Chunk* find(ChunkCoord coord) const noexcept override {
        const auto it = chunks_.find(coord);
        return it == chunks_.end() ? nullptr : it->second;
    }
    [[nodiscard]] Sample outside() const noexcept override {
        return outside_;
    }

private:
    std::unordered_map<ChunkCoord, const Chunk*, ChunkCoordHash> chunks_;
    Sample outside_;
};

// Erzeugt Netze für Blätter eines LOD-Baums mit Surface Nets (R3.3, ADR 0041). Wie Mesher, aber
// über Stufengrenzen hinweg geschlossen: Auf einer Fläche zwischen zwei Stufen gelten die Samples
// der groben Seite, Zwischenpunkte der feinen Seite werden linear interpoliert. Jede Gitterkante
// gehört der Zelle, die in beiden Querrichtungen oben liegt; an Stufengrenzen verbindet sie die
// Eckpunkte der Zellen beider Seiten zu Vierecken oder Dreiecken (Nahtpolygone). Positionen in
// Samples des Blatts relativ zu seiner Ecke. Ein LodMesher gehört einem Thread.
class LodMesher {
public:
    LodMesher();
    ~LodMesher();
    LodMesher(LodMesher&&) noexcept;
    LodMesher& operator=(LodMesher&&) noexcept;
    // Ersetzt out durch das Netz des Blatts leaf. layout muss ausgeglichen sein; store hält die
    // Samples aller Blätter (fehlende Blätter gelten als outside()).
    void build(const LodLayout& layout, const ChunkStore& store, ChunkCoord leaf, Mesh& out);
    // Wie oben, Samples aus source.
    void build(const LodLayout& layout, const ChunkSource& source, ChunkCoord leaf, Mesh& out);

private:
    // Zwischenspeicher hinter einem Zeiger, damit die Schnittstelle schlank bleibt.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Strahltest über die Blätter eines LOD-Baums (R3.4); Koordinaten in Voxeln relativ zum Ursprung
// des Terrains. Liest grobe Blätter auf ihrem Raster und geht dort in größeren Schritten. Außerhalb
// der Blätter ist Luft.
[[nodiscard]] std::optional<RayHit> raycast(const LodLayout& layout, const ChunkStore& store,
                                            Vec3 origin, Vec3 direction, f32 max_length);

// Netze vieler Blätter parallel im Job-System: out[i] wird zum Netz von leaves[i]. layout und
// store werden nur gelesen.
void mesh_leaves(jobs::JobSystem& jobs, const LodLayout& layout, const ChunkStore& store,
                 std::span<const ChunkCoord> leaves, std::span<Mesh> out);

} // namespace ngine::voxel
