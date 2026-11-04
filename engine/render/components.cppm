module;

#include <numbers>

export module ngine.render:components;

import ngine.core;
import ngine.reflect;
import ngine.rhi;

export namespace ngine::render {

// Kamera als Komponente (REND-08). Lage und Blickrichtung kommen aus WorldTransform: Die Kamera
// blickt entlang ihrer lokalen -z-Achse, +y ist oben. Gezeichnet wird von der ersten aktiven
// Kamera aus, mit umgekehrter Tiefe und unendlich fernem Horizont.
struct Camera {
    // Senkrechter Öffnungswinkel im Bogenmaß.
    f32 fov_y = 60.0f * std::numbers::pi_v<f32> / 180.0f;
    // Nahe Ebene in Metern; eine ferne gibt es nicht.
    f32 near_plane = 0.05f;
    // Faktor auf das HDR-Bild vor dem Tone Mapping.
    f32 exposure = 1.0f;
    bool active = true;
};

// Zeichnet ein Mesh aus dem MeshStore an der Lage von WorldTransform. Jedes Teilnetz bringt sein
// Material mit; material ersetzt es für alle Teilnetze. Die Indizes gelten nur für diesen
// Programmlauf, darum speichern Szenen die Komponente nicht (bis Assets in R2 feste Kennungen
// bekommen).
struct MeshRenderer {
    u32 mesh = rhi::invalid_index;
    u32 material = rhi::invalid_index;
};

// Registriert Camera und MeshRenderer in der Reflection (nach world::register_types()).
void register_types(reflect::TypeRegistry& types);

} // namespace ngine::render
