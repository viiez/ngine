module;

#include <compare>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module ngine.voxel:chunk;

import ngine.core;
import ngine.math;
import :grid;

export namespace ngine::voxel {

// Samples pro Achse, die ein Chunk besitzt (R3.1, ADR 0038). Das Sample bei 32 gehört schon dem
// Nachbarn; ein Chunk deckt also 32 Voxel pro Achse ab.
inline constexpr i32 chunk_voxels = 32;
inline constexpr usize chunk_sample_count = usize{32} * 32 * 32;
// Kantenlänge eines Voxels in Metern; ein Chunk ist damit 16 m breit.
inline constexpr f64 voxel_size = 0.5;
inline constexpr f64 chunk_size = voxel_size * chunk_voxels;

// Lage eines Chunks im Raster der Chunks. Stufe 0 sind die feinsten Chunks; ein Chunk der Stufe n
// deckt 32·2ⁿ Voxel ab, weiterhin mit 32³ Samples (ab R3.3, bis dahin gibt es nur Stufe 0).
struct ChunkCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    u8 level = 0;

    friend constexpr bool operator==(ChunkCoord, ChunkCoord) noexcept = default;
    friend constexpr auto operator<=>(ChunkCoord, ChunkCoord) noexcept = default;
};

struct ChunkCoordHash {
    [[nodiscard]] usize operator()(ChunkCoord c) const noexcept {
        u64 h = static_cast<u32>(c.x) * 0x9E3779B97F4A7C15ull;
        h ^= (static_cast<u32>(c.y) + 0x7F4A7C15ull + (h << 6) + (h >> 2)) * 0xBF58476D1CE4E5B9ull;
        h ^= (static_cast<u32>(c.z) + 0x94D049BBull + (h << 6) + (h >> 2)) * 0x94D049BB133111EBull;
        h ^= c.level;
        return static_cast<usize>(h ^ (h >> 31));
    }
};

// Abrundende Division: auch für negative Koordinaten der Chunk, in dem ein Sample liegt.
[[nodiscard]] constexpr i64 floor_div(i64 value, i64 divisor) noexcept {
    const i64 q = value / divisor;
    return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? q - 1 : q;
}

// Erstes Sample (Stufe 0) eines Chunks entlang einer Achse.
[[nodiscard]] constexpr i64 chunk_origin(i32 coord, u8 level = 0) noexcept {
    return i64{coord} * chunk_voxels * (i64{1} << level);
}

// Chunk der Stufe 0, dem das Sample gehört.
[[nodiscard]] constexpr ChunkCoord chunk_of(i64 x, i64 y, i64 z) noexcept {
    return {static_cast<i32>(floor_div(x, chunk_voxels)),
            static_cast<i32>(floor_div(y, chunk_voxels)),
            static_cast<i32>(floor_div(z, chunk_voxels)), 0};
}

// Ecke des Chunks in Metern relativ zum Ursprung des Terrains (64 Bit, ADR 0012).
[[nodiscard]] constexpr math::DVec3 chunk_position(ChunkCoord c) noexcept {
    const f64 scale = chunk_size * static_cast<f64>(i64{1} << c.level);
    return {static_cast<f64>(c.x) * scale, static_cast<f64>(c.y) * scale,
            static_cast<f64>(c.z) * scale};
}

// Achsenparalleler Bereich von Samples in Koordinaten des Terrains (Stufe 0), min inklusive,
// max exklusive.
struct SampleBox {
    i64 min_x = 0, min_y = 0, min_z = 0;
    i64 max_x = 0, max_y = 0, max_z = 0;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return min_x >= max_x || min_y >= max_y || min_z >= max_z;
    }
};

// 32³ Samples eines Chunks. Sind alle gleich (ganz Luft oder ganz fest), hält der Chunk nur einen
// Wert statt 64 KiB.
class Chunk {
public:
    // Ein Chunk aus Luft.
    Chunk() = default;
    explicit Chunk(Sample uniform) noexcept : uniform_{uniform} {}

    [[nodiscard]] static constexpr usize index(u32 x, u32 y, u32 z) noexcept {
        return (usize{z} * chunk_voxels + y) * chunk_voxels + x;
    }

    // Alle Samples gleich; dann gibt es kein Feld.
    [[nodiscard]] bool uniform() const noexcept {
        return samples_.empty();
    }
    // Der gemeinsame Wert, nur gültig, wenn uniform().
    [[nodiscard]] Sample uniform_value() const noexcept {
        return uniform_;
    }

    [[nodiscard]] Sample sample(u32 x, u32 y, u32 z) const noexcept {
        return samples_.empty() ? uniform_ : samples_[index(x, y, z)];
    }
    // Setzt ein Sample; ein einheitlicher Chunk wird dafür zum vollen Feld.
    void set(u32 x, u32 y, u32 z, Sample sample);
    // Setzt alle Samples auf einen Wert und gibt das Feld frei.
    void fill(Sample sample) noexcept;

    // Feld zum Lesen; leer, wenn der Chunk einheitlich ist.
    [[nodiscard]] std::span<const Sample> samples() const noexcept {
        return samples_;
    }
    // Feld zum Schreiben; legt es bei Bedarf an (alle Samples mit dem bisherigen Wert).
    [[nodiscard]] std::span<Sample> expand();
    // Gibt das Feld frei, wenn alle Samples gleich sind. Liefert, ob der Chunk danach einheitlich
    // ist.
    bool compact();

    // Speicher der Samples in Bytes (ohne Verwaltung).
    [[nodiscard]] usize memory_bytes() const noexcept {
        return samples_.capacity() * sizeof(Sample);
    }

private:
    Sample uniform_{};
    std::vector<Sample> samples_;
};

// Alle Chunks eines Terrains (VOX-01): Chunks nach Koordinate, Zugriff auf Samples in
// Koordinaten des Terrains und Buchführung, welche Netze sich geändert haben. Wo kein Chunk
// liegt, gilt das Sample outside (Luft). Ein ChunkStore gehört einem Thread; Jobs dürfen
// gleichzeitig lesen oder jeweils nur in ihren eigenen Chunk schreiben.
class ChunkStore {
public:
    explicit ChunkStore(Sample outside = {}) noexcept : outside_{outside} {}
    ChunkStore(const ChunkStore&) = delete;
    ChunkStore& operator=(const ChunkStore&) = delete;
    ChunkStore(ChunkStore&&) noexcept = default;
    ChunkStore& operator=(ChunkStore&&) noexcept = default;

    [[nodiscard]] Sample outside() const noexcept {
        return outside_;
    }

    [[nodiscard]] const Chunk* find(ChunkCoord coord) const noexcept;
    [[nodiscard]] Chunk* find(ChunkCoord coord) noexcept;
    // Liefert den Chunk; ein neuer besteht aus outside(). Markiert ihn nicht als geändert.
    Chunk& get_or_create(ChunkCoord coord);
    void remove(ChunkCoord coord);
    void clear();

    [[nodiscard]] usize size() const noexcept {
        return chunks_.size();
    }
    // Koordinaten aller Chunks, sortiert.
    [[nodiscard]] std::vector<ChunkCoord> coords() const;
    // Speicher aller Samples in Bytes.
    [[nodiscard]] usize memory_bytes() const noexcept;
    // Chunks, die nur einen Wert halten.
    [[nodiscard]] usize uniform_count() const noexcept;

    // Samples in Koordinaten des Terrains (Stufe 0).
    [[nodiscard]] Sample sample(i64 x, i64 y, i64 z) const noexcept;
    // Setzt ein Sample und legt den Chunk bei Bedarf an. Liefert, ob sich etwas geändert hat.
    // Markiert nichts; dafür mark_changed().
    bool set(i64 x, i64 y, i64 z, Sample sample);
    [[nodiscard]] f32 distance(i64 x, i64 y, i64 z) const noexcept {
        return dequantize(sample(x, y, z).density);
    }
    // Abstand in Voxeln, trilinear interpoliert; p in Voxeln relativ zum Ursprung des Terrains.
    [[nodiscard]] f32 distance_at(math::Vec3 p) const noexcept;

    // Merkt alle Chunks vor, deren Netz von Samples in box abhängt (eigene Samples plus
    // Randschicht, ADR 0038), auch solche ohne eigenen Chunk an den Rändern.
    void mark_changed(const SampleBox& box);
    // Alle Samples des Chunks haben sich geändert.
    void mark_changed(ChunkCoord coord);
    [[nodiscard]] bool has_changes() const noexcept {
        return !dirty_.empty();
    }
    // Vorgemerkte Chunks, sortiert; leert die Vormerkung.
    [[nodiscard]] std::vector<ChunkCoord> take_changes();
    // Erhöht sich bei jeder Vormerkung.
    [[nodiscard]] u64 version() const noexcept {
        return version_;
    }

private:
    Sample outside_;
    std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash> chunks_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> dirty_;
    u64 version_ = 0;
};

// Bereich der Samples eines Chunks samt Randschicht, wie ihn der Mesher liest: zwei Samples vor
// dem Chunk (eine Zelle für die Vierecke, eine für die Normalen) und zwei nach seinem letzten
// eigenen Sample (ADR 0038).
inline constexpr i32 border_before = 2;
inline constexpr i32 border_after = 2;
inline constexpr u32 padded_samples = chunk_voxels + border_before + border_after;

// Kopiert die Samples des Chunks samt Randschicht der 26 Nachbarn in out (padded_samples³).
// Fehlende Nachbarn liefern outside(). Nur Stufe 0.
void gather(const ChunkStore& store, ChunkCoord coord, Grid& out);

// Ob Chunk und Nachbarn alle einheitlich (oder nicht vorhanden) sind und auf derselben Seite der
// Oberfläche liegen; dann hat der Chunk kein Netz und muss nicht gesammelt werden.
[[nodiscard]] bool trivially_empty(const ChunkStore& store, ChunkCoord coord);

} // namespace ngine::voxel
