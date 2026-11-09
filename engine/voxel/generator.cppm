module;

#include <span>

export module ngine.voxel:generator;

import ngine.core;
import ngine.jobs;
import :grid;
import :chunk;
import :noise;

export namespace ngine::voxel {

// Nummern der Materialien in der Standardtabelle (0 ist Luft).
inline constexpr Material grass = 1;
inline constexpr Material rock = 2;
inline constexpr Material earth = 3;
inline constexpr Material sand = 4;

// Regeln der prozeduralen Grundwelt (VOX-05, VOX-09, R3.2, ADR 0039). Längen und Höhen in
// Metern, Höhen über der Unterkante des Terrains (Sample y = 0). Ein Ausschlag von 0 schaltet die
// Regel ab.
struct GeneratorDesc {
    u64 seed = 1;
    // Mittlere Höhe der Oberfläche.
    f32 ground = 20.0f;
    // Hügel aus 2D-fBm: Ausschlag und Wellenlänge.
    f32 hills = 6.0f;
    f32 hill_scale = 96.0f;
    // Felsrücken aus Gratrauschen, nur wo eine grobe Maske sie zulässt: Höhe und Wellenlänge.
    f32 mountains = 24.0f;
    f32 mountain_scale = 256.0f;
    // Verschiebt die Eingabe des Höhenrauschens um ein weiteres Rauschen (Domain Warping), damit
    // Hügel und Grate nicht wie Rauschen aussehen.
    f32 warp = 20.0f;
    // Terrassen auf den Felsrücken, deren steile Stufen zu Felswänden werden: Stärke 0 bis 1 und
    // Höhe einer Stufe.
    f32 cliffs = 0.7f;
    f32 terrace = 5.0f;
    // 3D-Rauschen auf der Dichte an steilen Stellen: Überhänge und zerklüftete Wände.
    f32 overhangs = 2.5f;
    // Höhlen: 0 keine, 1 viele. Gänge aus dem Schnitt zweier 3D-Rauschfelder, Kammern aus einem
    // dritten, erst ab einigen Metern unter der Oberfläche.
    f32 caves = 0.5f;
    f32 cave_scale = 48.0f;
    // Unterhalb dieser Höhe liegt Sand an der Oberfläche.
    f32 sand_level = 14.0f;
    // Dicke der Schichten unter der Oberfläche: Gras (oder Sand), darunter Erde, dann Fels.
    f32 grass_depth = 0.75f;
    f32 earth_depth = 3.0f;
};

// Erzeugt Samples der Grundwelt als reine Funktion aus Startwert und Position: Server und Clients
// erzeugen dieselbe Welt, bitgleich auf allen Plattformen (ADR 0039). Konstant nach dem Anlegen,
// also aus vielen Threads gleichzeitig benutzbar.
class Generator {
public:
    explicit Generator(const GeneratorDesc& desc = {});

    [[nodiscard]] const GeneratorDesc& desc() const noexcept {
        return desc_;
    }

    // Füllt einen Chunk der Stufe 0 und schrumpft ihn, wenn er einheitlich ist.
    void fill(ChunkCoord coord, Chunk& out) const;
    // Ein einzelnes Sample in Koordinaten des Terrains; gleich dem, was fill() dort schreibt.
    [[nodiscard]] Sample sample(i64 x, i64 y, i64 z) const;
    // Höhe der Oberfläche aus den 2D-Regeln in Voxeln (ohne Überhänge und Höhlen).
    [[nodiscard]] f64 height(i64 x, i64 z) const noexcept;

    // Abstand der Gitterpunkte, an denen die 3D-Regeln ausgewertet und dazwischen trilinear
    // interpoliert werden (in Voxeln).
    static constexpr i64 lattice = 4;

private:
    struct Column;
    struct Lattice;
    [[nodiscard]] Column column(f64 h, f64 h_x0, f64 h_x1, f64 h_z0, f64 h_z1) const noexcept;
    [[nodiscard]] Lattice lattice_point(i64 lx, i64 ly, i64 lz) const noexcept;
    [[nodiscard]] Sample evaluate(const Column& column, const Lattice& field, i64 y) const noexcept;
    [[nodiscard]] bool needs_lattice() const noexcept;

    GeneratorDesc desc_;
    // Ausschläge und Maßstäbe in Voxeln.
    f64 ground_ = 0.0, hills_ = 0.0, hill_scale_ = 1.0, mountains_ = 0.0, mountain_scale_ = 1.0;
    f64 warp_ = 0.0, terrace_ = 0.0, overhangs_ = 0.0, cave_scale_ = 1.0, sand_level_ = 0.0;
    f64 grass_depth_ = 0.0, earth_depth_ = 0.0;
    Noise warp_x_, warp_z_, hill_noise_, mask_noise_, ridge_noise_;
    Noise overhang_noise_, tunnel_a_, tunnel_b_, cavern_noise_;
};

// Füllt die Chunks coords (legt sie bei Bedarf an) aus dem Generator, parallel im Job-System;
// jeder Job schreibt nur in seinen Chunk. Alle betroffenen Netze sind danach vorgemerkt.
void generate_chunks(jobs::JobSystem& jobs, ChunkStore& store, std::span<const ChunkCoord> coords,
                     const Generator& generator);

// Prüfsumme (FNV-1a, 64 Bit) über alle Samples eines Chunks bzw. aller Chunks in Reihenfolge der
// Koordinaten; einheitliche Chunks zählen wie volle. Für Tests auf Bitgleichheit.
[[nodiscard]] u64 checksum(const Chunk& chunk) noexcept;
[[nodiscard]] u64 checksum(const ChunkStore& store);

} // namespace ngine::voxel
