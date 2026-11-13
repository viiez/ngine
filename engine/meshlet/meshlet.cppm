module;

#include <cstddef>
#include <span>
#include <vector>

// Meshlets (R3.5, ADR 0043): kleine Gruppen von Dreiecken mit Hülle, die die GPU einzeln prüft und
// zeichnet. Ein Netz wird dafür umsortiert: Die Dreiecke eines Meshlets liegen hintereinander im
// Indexpuffer. So zeichnet der Rückfallpfad ein Meshlet als Bereich des Indexpuffers, und der
// Mesh-Shader-Pfad liest dieselben Dreiecke über die lokalen Daten (local_data()).
export module ngine.meshlet;

import ngine.core;
import ngine.math;

export namespace ngine::meshlet {

// Grenzen eines Meshlets; sie passen zu jeder GPU mit Mesh Shadern (Vulkan verlangt mindestens 256
// Eckpunkte und Dreiecke pro Arbeitsgruppe) und zu einem Byte je lokalem Index.
inline constexpr u32 max_vertices = 64;
inline constexpr u32 max_triangles = 124;

// Ein Meshlet: Dreiecke [first_triangle, first_triangle + triangle_count) des umsortierten
// Indexpuffers mit vertex_count verschiedenen Eckpunkten. Hüllkugel und Normalenkegel in den
// Koordinaten der Eckpunkte. Der Kegel erlaubt, Meshlets auszusortieren, deren Dreiecke alle vom
// Betrachter wegzeigen: Von V aus ist das Meshlet abgewandt, wenn
// dot(center - V, cone_axis) >= cone_cutoff * |center - V| + radius. cone_cutoff 1 heißt: nie.
struct Meshlet {
    u32 first_triangle = 0;
    u32 triangle_count = 0;
    u32 vertex_count = 0;
    math::Vec3 center;
    f32 radius = 0.0f;
    math::Vec3 cone_axis;
    f32 cone_cutoff = 1.0f;

    friend bool operator==(const Meshlet&, const Meshlet&) = default;
};

// Positionen von Eckpunkten beliebiger Art: drei f32 am Anfang jedes Eckpunkts, stride Bytes von
// einem Eckpunkt zum nächsten (voxel::Vertex, asset::Vertex).
struct Positions {
    const std::byte* data = nullptr;
    usize stride = 0;
    usize count = 0;

    [[nodiscard]] math::Vec3 operator[](usize index) const noexcept;
};

template <typename Vertex>
[[nodiscard]] Positions positions_of(std::span<const Vertex> vertices) noexcept {
    return {reinterpret_cast<const std::byte*>(vertices.data()), sizeof(Vertex), vertices.size()};
}

// Teilt ein Netz in Meshlets und sortiert dafür seine Dreiecke um (indices, drei je Dreieck). Die
// Dreiecke werden wie in einem k-d-Baum geteilt: am Median ihrer Mittelpunkte entlang der längsten
// Achse, bis ein Bereich in ein Meshlet passt; so bleiben Meshlets räumlich kompakt.
// Deterministisch und ohne Fremdbibliothek.
[[nodiscard]] std::vector<Meshlet> build(Positions positions, std::span<u32> indices);

// Hülle und Kegel eines Meshlets aus seinen Dreiecken; first_triangle, triangle_count und
// vertex_count bleiben, wie sie sind. Für Meshlets aus anderen Quellen (meshoptimizer beim
// Kochen), damit alle Hüllen gleich entstehen.
void compute_bounds(Positions positions, std::span<const u32> indices, Meshlet& meshlet);

// Prüft, ob Meshlets ein umsortiertes Netz lückenlos und innerhalb der Grenzen abdecken.
[[nodiscard]] bool valid(std::span<const Meshlet> meshlets, std::span<const u32> indices);

// Lokale Daten für Mesh Shader: je Meshlet die Liste seiner Eckpunkte (Indizes ins Netz) und je
// Dreieck drei lokale Indizes in einem Wort (Bits 0–7, 8–15, 16–23). Die Dreiecke stehen in
// derselben Reihenfolge wie im umsortierten Indexpuffer; vertex_offsets nennt je Meshlet den
// Anfang seiner Eckpunkte in vertices.
struct LocalData {
    std::vector<u32> vertices;
    std::vector<u32> triangles;
    std::vector<u32> vertex_offsets;
};
[[nodiscard]] LocalData local_data(std::span<const Meshlet> meshlets, std::span<const u32> indices);

} // namespace ngine::meshlet
