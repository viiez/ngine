module;

#include <numbers>

export module ngine.render:components;

import ngine.core;
import ngine.math;
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

// Lichter (REND-09), angelehnt an glTF KHR_lights_punctual. Lage und Richtung kommen aus
// WorldTransform: Lichter leuchten entlang ihrer lokalen -z-Achse. Die Stärke ist eine Zahl ohne
// feste Einheit, abgestimmt auf die Belichtung der Kamera (1 = Sonne bei Belichtung 1 knapp
// unter Weiß).

// Paralleles Licht aus unendlicher Ferne. Das erste aktive ist die Sonne und wirft Schatten über
// Cascaded Shadow Maps (wenn cast_shadows); weitere leuchten ohne Schatten.
struct DirectionalLight {
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    f32 intensity = 3.0f;
    bool cast_shadows = true;
    bool active = true;
};

// Punktlicht: Abfall mit dem Quadrat der Entfernung, am Rand der Reichweite weich auf null.
// Reichweite 0 heißt unbegrenzt. Wirft keine Schatten.
struct PointLight {
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    f32 intensity = 10.0f;
    f32 range = 10.0f;
    bool active = true;
};

// Spotlicht: wie ein Punktlicht, aber auf einen Kegel um -z begrenzt. Innerhalb von inner_angle
// volle Stärke, bis outer_angle (halber Öffnungswinkel, Bogenmaß) weich auf null.
struct SpotLight {
    math::Vec3 color{1.0f, 1.0f, 1.0f};
    f32 intensity = 10.0f;
    f32 range = 10.0f;
    f32 inner_angle = 0.0f;
    f32 outer_angle = std::numbers::pi_v<f32> / 4.0f;
    bool active = true;
};

// Registriert Camera, MeshRenderer und die Lichter in der Reflection (nach
// world::register_types()).
void register_types(reflect::TypeRegistry& types);

} // namespace ngine::render
