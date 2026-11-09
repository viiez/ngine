module;

#include <map>
#include <memory>
#include <optional>

export module ngine.world:terrain;

import ngine.core;
import ngine.jobs;
import ngine.reflect;
export import ngine.voxel;
import :ecs;

export namespace ngine::world {

// Welche Regeln des Generators gelten (voxel::GeneratorDesc, R3.2).
enum class TerrainShape : u8 {
    // Ebene in Höhe ground.
    Flat,
    // Sanfte Hügel um ground mit Ausschlag height, ohne Felsen und Höhlen.
    Hills,
    // Alle Regeln: Hügel, Felsrücken mit Felswänden, Überhänge, Höhlen (VOX-05, VOX-09).
    Procedural,
};

// Voxel-Terrain als Komponente (VOX-01, R3.1, ADR 0038). Die Szene speichert nur, wie das Terrain
// entsteht; Chunks und Netze hält TerrainState zur Laufzeit. Das Terrain beginnt an der Position
// der Entity (Drehung und Skalierung zählen nicht, Chunks liegen achsenparallel) und reicht über
// size_x × size_y × size_z Chunks zu je 16 m, in x und z um die Position zentriert. Seit R3.2
// entstehen die Chunks aus dem Generator mit Startwert (ADR 0039); Höhen in Metern über der
// Unterkante.
struct Terrain {
    i32 size_x = 8;
    i32 size_y = 4;
    i32 size_z = 8;
    TerrainShape shape = TerrainShape::Procedural;
    // Startwert: Gleicher Wert, gleiche Welt auf jedem Rechner.
    u32 seed = 1;
    // Mittlere Höhe der Oberfläche.
    f32 ground = 20.0f;
    // Ausschlag der Hügel.
    f32 height = 6.0f;
    // Höhe der Felsrücken (nur Procedural).
    f32 mountains = 24.0f;
    // Ausschlag der Überhänge an steilen Stellen (nur Procedural).
    f32 overhangs = 2.5f;
    // Menge der Höhlen von 0 bis 1 (nur Procedural).
    f32 caves = 0.5f;
    // Unterhalb dieser Höhe liegt Sand an der Oberfläche.
    f32 sand_level = 14.0f;

    friend bool operator==(const Terrain&, const Terrain&) noexcept = default;
};

// Regeln des Generators für ein Terrain.
[[nodiscard]] voxel::GeneratorDesc generator_desc(const Terrain& terrain) noexcept;

// Netz eines Chunks auf der CPU; version wächst mit jeder neuen Fassung.
struct ChunkMesh {
    voxel::Mesh mesh;
    u64 version = 0;
};

// Messwerte des letzten Durchlaufs von update_terrains() für dieses Terrain.
struct TerrainStats {
    // Chunks mit Samples und davon einheitliche (ein Wert).
    u32 chunks = 0;
    u32 uniform_chunks = 0;
    // Chunks mit Netz, Dreiecke aller Netze.
    u32 meshes = 0;
    u32 triangles = 0;
    // Im letzten Durchlauf neu gemeshte Chunks und die Zeit dafür.
    u32 meshed = 0;
    f64 mesh_ms = 0.0;
    // Zeit für das letzte Erzeugen aller Chunks, insgesamt und pro Chunk.
    f64 generate_ms = 0.0;
    f64 generate_chunk_ms = 0.0;
    // Speicher der Samples.
    usize sample_bytes = 0;
};

// Laufzeitdaten eines Terrains: Chunks, Netze und Messwerte. Gehört zu TerrainState und wird
// zwischen Welt (schreibt in update_terrains() und über Pinsel) und Renderer (liest) geteilt.
struct TerrainData {
    voxel::ChunkStore chunks;
    // Netze aller Chunks, die eines haben (auch Chunks am Rand ohne eigene Samples).
    std::map<voxel::ChunkCoord, ChunkMesh> meshes;
    // Eindeutig im Programmlauf, z. B. als Schlüssel für GPU-Daten im Renderer.
    u64 id = 0;
    // Beschreibung, aus der die Chunks entstanden sind; ändert sich Terrain, entsteht alles neu.
    std::optional<Terrain> generated;
    TerrainStats stats;
};

// Laufzeitkomponente (transient) zu Terrain; update_terrains() legt sie an. Sie hat keine
// reflektierten Felder: Nach dem Laden oder Spielen im Editor entsteht das Terrain neu aus
// Terrain.
struct TerrainState {
    std::shared_ptr<TerrainData> data;
};

// Ursprung des Terrains in Voxeln relativ zur Position der Entity (Ecke des Chunks mit der
// kleinsten Koordinate liegt bei chunk_origin(first)).
[[nodiscard]] voxel::ChunkCoord terrain_first_chunk(const Terrain& terrain) noexcept;

// Erzeugt alle Chunks des Terrains neu aus dem Generator, parallel im Job-System.
void generate_terrain(const Terrain& terrain, voxel::ChunkStore& chunks, jobs::JobSystem& jobs);

// Hält alle Terrains der Welt aktuell: legt TerrainState an, erzeugt die Chunks neu, wenn sich
// Terrain geändert hat, und meshed geänderte Chunks parallel im Job-System (VOX-02). Läuft wie
// update_transforms() nach Schedule::run(). Liefert die Zahl der neu gemeshten Chunks.
usize update_terrains(World& world, jobs::JobSystem& jobs);

// Registriert Terrain, TerrainShape und TerrainState in der Reflection (aus register_types()).
void register_terrain_types(reflect::TypeRegistry& types);

} // namespace ngine::world
