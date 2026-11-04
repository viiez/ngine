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

} // namespace ngine::asset
