module;

#include <vector>

export module ngine.voxel:mesh;

import ngine.core;
import :grid;

export namespace ngine::voxel {

// Ein Eckpunkt, so wie ihn die GPU liest (32 Bytes, zwei float4). Position in Voxeln relativ zum
// Ursprung des Gitters, Normale zeigt nach außen.
struct Vertex {
    f32 px = 0.0f, py = 0.0f, pz = 0.0f;
    u32 material = 0;
    f32 nx = 0.0f, ny = 0.0f, nz = 0.0f;
    f32 reserved = 0.0f;
};
static_assert(sizeof(Vertex) == 32);

// Dreiecksnetz mit Indizes, Dreiecke gegen den Uhrzeigersinn von außen gesehen.
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    [[nodiscard]] usize triangle_count() const noexcept {
        return indices.size() / 3;
    }
    void clear() noexcept {
        vertices.clear();
        indices.clear();
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
