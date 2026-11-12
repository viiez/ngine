module;

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module ngine.voxel:delta;

import ngine.core;
import :grid;
import :chunk;
import :edit;
import :generator;

// Änderungen am Terrain als Deltas gegenüber der prozeduralen Grundwelt (VOX-06, R3.4, ADR 0042).
// Nur Chunks der Stufe 0 haben Deltas; grobe Stufen rechnen sie beim Erzeugen ein. So überleben
// Änderungen das Entladen, und gespeichert werden nur sie, nicht die Welt.
export namespace ngine::voxel {

template <typename T>
using Result = std::expected<T, std::string>;

// Die geänderten Samples eines Chunks der Stufe 0, kompakt kodiert: abwechselnd die Zahl
// unveränderter Samples und die Zahl geänderter, beide als Varint (LEB128), nach jeder Zahl
// geänderter Samples deren neue Werte (Dichte, Material; je 2 Bytes). Reihenfolge wie
// Chunk::index(). Unveränderte Samples am Ende fehlen. Ein Krater kostet so nur seine Oberfläche
// und sein Inneres, nicht den ganzen Chunk. Unveränderlich, also aus vielen Threads lesbar.
class ChunkDelta {
public:
    ChunkDelta() = default;

    // Delta von base (Grundwelt) zu edited; leer, wenn beide gleich sind.
    [[nodiscard]] static ChunkDelta diff(const Chunk& base, const Chunk& edited);
    // Aus kodierten Bytes; prüft, dass alles in einen Chunk passt.
    [[nodiscard]] static Result<ChunkDelta> decode(std::span<const std::byte> bytes);

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return changed_ == 0;
    }
    // Zahl der geänderten Samples.
    [[nodiscard]] u32 changed() const noexcept {
        return changed_;
    }
    // Setzt die geänderten Samples in chunk (Stufe 0) und schrumpft ihn, wenn er einheitlich wird.
    void apply(Chunk& chunk) const;
    // Ruft visit(index, sample) für jedes geänderte Sample auf, aufsteigend nach Chunk::index().
    template <typename F>
    void for_each(F&& visit) const {
        usize pos = 0;
        usize index = 0;
        while (pos < bytes_.size()) {
            index += read_varint(pos);
            const usize count = read_varint(pos);
            for (usize i = 0; i < count; ++i, ++index, pos += 2) {
                visit(index,
                      Sample{static_cast<i8>(bytes_[pos]), static_cast<Material>(bytes_[pos + 1])});
            }
        }
    }

    friend bool operator==(const ChunkDelta& a, const ChunkDelta& b) noexcept {
        return a.bytes_ == b.bytes_;
    }

private:
    [[nodiscard]] usize read_varint(usize& pos) const noexcept;

    std::vector<std::byte> bytes_;
    u32 changed_ = 0;
};

// Deltas aller Chunks eines Terrains nach Koordinate (Stufe 0). Einträge sind unveränderlich und
// werden geteilt: Befehle für Undo halten alte und neue Fassungen, Jobs eine Momentaufnahme.
using DeltaMap = std::unordered_map<ChunkCoord, std::shared_ptr<const ChunkDelta>, ChunkCoordHash>;

// Rechnet die Deltas in einen frisch aus dem Generator gefüllten Chunk beliebiger Stufe ein. Auf
// Stufe 0 gelten die geänderten Samples. Ein grober Chunk übernimmt ein geändertes Sample der
// Stufe 0, das auf seinem Raster liegt: Nahe der Oberfläche (Abstand unter max_distance) dessen
// Abstand in seinen Schritten; weiter weg bleibt sein eigener Wert, wenn er auf derselben Seite
// liegt, sonst der größte Abstand, den die Stufe 0 kennt. Was kleiner als ein Schritt der Stufe
// ist, verschwindet in der Ferne wie beim Generator.
void apply_deltas(const DeltaMap& deltas, ChunkCoord coord, Chunk& chunk);

// Änderung eines Chunks: Delta vorher und nachher (nullptr: keines). Grundlage für Undo.
struct DeltaChange {
    ChunkCoord coord;
    std::shared_ptr<const ChunkDelta> before;
    std::shared_ptr<const ChunkDelta> after;
};

// Was ein Pinsel geändert hat: Bereich der Samples und Deltas pro Chunk.
struct TerrainEdit {
    SampleBox box;
    std::vector<DeltaChange> changes;

    [[nodiscard]] bool empty() const noexcept {
        return changes.empty();
    }
};

// Die Deltas eines Terrains mit dem Generator seiner Grundwelt (VOX-06, R3.4). Gehört einem
// Thread; Jobs lesen nur Momentaufnahmen (snapshot()), die sich nie mehr ändern. Ändert sich ein
// Delta, während jemand eine Momentaufnahme hält, kopiert TerrainDeltas die Tabelle (Kopie beim
// Schreiben).
class TerrainDeltas {
public:
    explicit TerrainDeltas(const GeneratorDesc& generator = {});
    TerrainDeltas(const TerrainDeltas&) = delete;
    TerrainDeltas& operator=(const TerrainDeltas&) = delete;

    // Neue Grundwelt; die Deltas bleiben.
    void set_generator(const GeneratorDesc& generator);
    [[nodiscard]] const Generator& generator() const noexcept {
        return generator_;
    }

    // Alle Deltas, unveränderlich; nie nullptr.
    [[nodiscard]] std::shared_ptr<const DeltaMap> snapshot() const noexcept {
        return map_;
    }
    // Ersetzt alle Deltas (Laden, Ende des Spielens). nullptr leert.
    void assign(std::shared_ptr<const DeltaMap> deltas);
    void clear();
    [[nodiscard]] std::shared_ptr<const ChunkDelta> find(ChunkCoord coord) const;
    // Chunks mit Delta und Größe aller kodierten Deltas in Bytes.
    [[nodiscard]] usize size() const noexcept {
        return map_->size();
    }
    [[nodiscard]] usize encoded_bytes() const noexcept;
    // Erhöht sich mit jeder Änderung.
    [[nodiscard]] u64 version() const noexcept {
        return version_;
    }

    // Füllt einen Chunk beliebiger Stufe aus Grundwelt und Deltas.
    void build(ChunkCoord coord, Chunk& out) const;

    // Wendet den Pinsel auf die Samples der Stufe 0 an (Mitte in Voxeln relativ zum Ursprung des
    // Terrains). Chunks der Stufe 0 im store ändert er dort und merkt ihre Netze vor; fehlende
    // entstehen für die Rechnung aus Grundwelt und Deltas, außer mit loaded_only (Terrain fester
    // Größe: außerhalb gibt es nichts). Liefert die neuen Deltas, leer ohne Änderung.
    TerrainEdit apply(ChunkStore& store, const Brush& brush, bool loaded_only = false);
    // Setzt die Deltas der Chunks auf after (bzw. before) und baut ihre Chunks im store neu (Undo,
    // Redo). Liefert den Bereich der geänderten Chunks, leer, wenn schon alles so war.
    SampleBox restore(ChunkStore& store, std::span<const DeltaChange> changes, bool after);

private:
    DeltaMap& writable();
    void put(ChunkCoord coord, std::shared_ptr<const ChunkDelta> delta);
    // Chunk der Grundwelt (Stufe 0); die letzten werden zwischengespeichert, weil ein Pinselstrich
    // über viele Frames dieselben Chunks ändert.
    const Chunk& base(ChunkCoord coord);

    Generator generator_;
    std::shared_ptr<DeltaMap> map_;
    u64 version_ = 0;
    std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash> base_cache_;
    std::vector<ChunkCoord> base_order_;
};

// --- Regionen-Dateien ------------------------------------------------------------------------
// Deltas liegen in Regionen von 16 × 16 × 16 Chunks, eine Datei pro Region mit Deltas, im Ordner
// des Terrains neben der Szene (R3.4, ADR 0042). Format (Little Endian): „NGDR“, Version (u32),
// Zahl der Chunks (u32), dann je Chunk die Lage in der Region (3 × u8), Länge des Deltas (u32) und
// seine Bytes, nach Koordinaten sortiert. Dieselben Deltas ergeben dieselben Bytes.

inline constexpr i32 region_chunks = 16;
inline constexpr u32 region_version = 1;
inline constexpr std::string_view region_extension = ".ndelta";

struct RegionCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;

    friend constexpr bool operator==(RegionCoord, RegionCoord) noexcept = default;
};

[[nodiscard]] constexpr RegionCoord region_of(ChunkCoord c) noexcept {
    return {static_cast<i32>(floor_div(c.x, region_chunks)),
            static_cast<i32>(floor_div(c.y, region_chunks)),
            static_cast<i32>(floor_div(c.z, region_chunks))};
}

// Dateiname einer Region, z. B. „r.-1.0.2.ndelta“, und zurück.
[[nodiscard]] std::string region_file_name(RegionCoord region);
[[nodiscard]] std::optional<RegionCoord> parse_region_file_name(std::string_view name);

// Kodiert alle Regionen mit Deltas, nach Regionen sortiert.
[[nodiscard]] std::vector<std::pair<RegionCoord, std::vector<std::byte>>>
encode_regions(const DeltaMap& deltas);
// Liest eine Region und fügt ihre Deltas in out ein. Bei einem Fehler bleibt out unverändert.
[[nodiscard]] Result<usize> decode_region(RegionCoord region, std::span<const std::byte> bytes,
                                          DeltaMap& out);

} // namespace ngine::voxel
