module;

#include <span>

export module ngine.asset:geometry;

import ngine.core;
import :model;

export namespace ngine::asset {

// Normalen aus den Dreiecken, nach Fläche gewichtet gemittelt.
void generate_normals(std::span<Vertex> vertices, std::span<const u32> indices);

// Tangenten pro Dreieck aus Positionen und Texturkoordinaten, je Eckpunkt gemittelt und zur
// Normale orthogonalisiert. Kein MikkTSpace: Normalenkarten, die damit gebacken wurden, können an
// UV-Nähten leicht abweichen. Ohne brauchbare Texturkoordinaten wird eine beliebige Senkrechte
// zur Normale gewählt.
void generate_tangents(std::span<Vertex> vertices, std::span<const u32> indices);

// Kugel um den Ursprung mit Breiten- und Längenkreisen, Dreiecke gegen den Uhrzeigersinn von
// außen gesehen. u läuft einmal um die y-Achse, v vom Nord- (0) zum Südpol (1).
[[nodiscard]] Primitive make_sphere(f32 radius, u32 segments = 48, u32 rings = 24);

} // namespace ngine::asset
