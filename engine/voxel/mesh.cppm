module;

#include <array>
#include <vector>

export module ngine.voxel:mesh;

import ngine.core;
export import ngine.meshlet;
import :grid;

export namespace ngine::voxel {

// Plätze in der Materialpalette eines Netzes (R3.2, ADR 0040). Ein Netz kann bis zu so viele
// verschiedene Materialien zeigen; jeder Eckpunkt hat ein Gewicht pro Platz.
inline constexpr u32 palette_size = 8;

// Ein Eckpunkt, so wie ihn die GPU liest (32 Bytes, zwei float4). Position in Voxeln relativ zum
// Ursprung des Gitters, Normale zeigt nach außen. Die Gewichte der acht Palettenplätze (je 8 Bit,
// zusammen 255) verlaufen über das Dreieck; so gehen Materialien weich ineinander über.
struct Vertex {
    f32 px = 0.0f, py = 0.0f, pz = 0.0f;
    // Gewichte der Plätze 0 bis 3, Platz 0 im untersten Byte.
    u32 weights_low = 0;
    f32 nx = 0.0f, ny = 0.0f, nz = 0.0f;
    // Gewichte der Plätze 4 bis 7.
    u32 weights_high = 0;

    // Gewicht eines Palettenplatzes (0 bis 255).
    [[nodiscard]] constexpr u32 weight(u32 slot) const noexcept {
        const u32 word = slot < 4 ? weights_low : weights_high;
        return (word >> ((slot % 4) * 8)) & 0xFFu;
    }
};
static_assert(sizeof(Vertex) == 32);

// Dreiecksnetz mit Indizes, Dreiecke gegen den Uhrzeigersinn von außen gesehen. palette ordnet den
// Plätzen der Gewichte Materialien zu (die ersten palette_count gelten).
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    std::array<Material, palette_size> palette{};
    u32 palette_count = 0;
    // Meshlets über die Dreiecke (R3.5, ADR 0043); indices ist danach umsortiert.
    std::vector<meshlet::Meshlet> meshlets;
    // Lokale Daten der Meshlets für den Mesh-Shader, im Job gerechnet statt beim Hochladen.
    meshlet::LocalData local;

    [[nodiscard]] usize triangle_count() const noexcept {
        return indices.size() / 3;
    }
    void clear() noexcept {
        vertices.clear();
        indices.clear();
        palette = {};
        palette_count = 0;
        meshlets.clear();
        local = {};
    }
    // Material mit dem größten Gewicht an einem Eckpunkt.
    [[nodiscard]] Material dominant(const Vertex& vertex) const noexcept {
        u32 best = 0;
        for (u32 slot = 1; slot < palette_count; ++slot) {
            if (vertex.weight(slot) > vertex.weight(best)) {
                best = slot;
            }
        }
        return palette_count > 0 ? palette[best] : air;
    }
    // Gewicht eines Materials an einem Eckpunkt (0 bis 255).
    [[nodiscard]] u32 weight(const Vertex& vertex, Material material) const noexcept {
        for (u32 slot = 0; slot < palette_count; ++slot) {
            if (palette[slot] == material) {
                return vertex.weight(slot);
            }
        }
        return 0;
    }
};

// Erzeugt Netze mit Surface Nets (ADR 0021): ein Eckpunkt pro Voxel, das die Oberfläche schneidet,
// ein Viereck pro Kante mit Vorzeichenwechsel. Hält Zwischenspeicher, damit wiederholtes Meshing
// nach Änderungen nicht neu anfordert. Ein Mesher gehört einem Thread.
class Mesher {
public:
    // Ersetzt den Inhalt von out durch das Netz des ganzen Gitters. Am Rand bleibt das Netz
    // offen.
    void build(const Grid& grid, Mesh& out);

    // Netz eines Chunks aus seinen Samples samt Randschicht (gather(), padded_samples³). Der
    // Chunk vermascht genau die Gitterkanten, die an seinen eigenen Samples beginnen; die
    // Nachbarn vermaschen den Rest. So schließen Chunks nahtlos aneinander an (ADR 0038).
    // Positionen in Voxeln relativ zur Ecke des Chunks.
    void build_chunk(const Grid& padded, Mesh& out);

private:
    // Zellen [cell_min, cell_max) pro Achse bekommen Eckpunkte; Vierecke entstehen an Kanten,
    // deren Anfang entlang der Kante mindestens edge_min ist. offset wird von den Positionen
    // abgezogen.
    void build_range(const Grid& grid, Mesh& out, u32 cell_min, u32 cell_max, u32 edge_min,
                     f32 offset);

    std::vector<u32> cell_vertex_;
};

} // namespace ngine::voxel
