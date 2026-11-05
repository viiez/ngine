module;

#include <array>

// Cascaded Shadow Maps für die Sonne (REND-09): Aufteilung des Kamerastumpfs, Einpassen jeder
// Kaskade und Einrasten auf ganze Texel. Reine Rechnung ohne GPU, damit sie testbar ist.
export module ngine.render:shadows;

import ngine.core;
import ngine.math;

export namespace ngine::render {

inline constexpr u32 cascade_count = 4;

struct ShadowSettings {
    bool enabled = true;
    // Bis zu dieser Entfernung von der Kamera (entlang der Blickrichtung) gibt es Schatten.
    f32 distance = 80.0f;
    // Kantenlänge jeder Schattenkarte in Texeln.
    u32 resolution = 2048;
    // 0 teilt gleichmäßig, 1 logarithmisch.
    f32 split_lambda = 0.75f;
    // Anteil am Ende jeder Kaskade, über den sie weich in die nächste übergeht.
    f32 blend = 0.15f;
    // Versatz des gelesenen Punkts entlang der Normale, in Texeln der Kaskade.
    f32 normal_offset = 1.5f;
    // Tiefenversatz der Schatten-Pipeline (konstant und nach Steigung).
    f32 depth_bias_constant = 1.0f;
    f32 depth_bias_slope = 2.0f;
    // Wie weit Werfer zwischen Kaskade und Sonne noch erfasst werden, in Metern.
    f32 caster_distance = 200.0f;
};

// Entfernungen (entlang der Blickrichtung), an denen die Kaskaden enden; die letzte ist
// distance. Gemischt aus logarithmischer und gleichmäßiger Aufteilung.
[[nodiscard]] std::array<f32, cascade_count> cascade_splits(f32 near_plane, f32 distance,
                                                            f32 lambda);

struct Cascade {
    // Von kamerarelativen Weltkoordinaten in den Raum der Schattenkarte: x, y in [-1, 1],
    // Tiefe in [0, 1] (0 an der Sonne).
    math::Mat4 view_projection;
    // Hier endet die Kaskade (Entfernung entlang der Blickrichtung).
    f32 split_far = 0.0f;
    // Hier beginnt der Übergang in die nächste Kaskade.
    f32 blend_start = 0.0f;
    // Kantenlänge eines Texels in Metern.
    f32 texel_size = 0.0f;
    // Mittelpunkt (kamerarelativ) und Achsen des Lichtraums; axis_z zeigt zur Sonne.
    math::Vec3 center;
    math::Vec3 axis_x;
    math::Vec3 axis_y;
    math::Vec3 axis_z;
    f32 radius = 0.0f;
    // Zusätzliche Tiefe Richtung Sonne für Werfer außerhalb der Kaskade.
    f32 caster_distance = 0.0f;

    // Ob eine Kugel (kamerarelativ) Schatten in diese Kaskade werfen kann.
    [[nodiscard]] bool may_cast(math::Vec3 sphere_center, f32 sphere_radius) const noexcept;
};

struct CascadeView {
    math::DVec3 position;
    // Spalten: rechts, oben, nach hinten (Blick entlang -z), wie render::CameraView.
    math::Mat3 rotation;
    f32 fov_y = 1.0f;
    f32 aspect = 1.0f;
    f32 near_plane = 0.1f;
};

// Passt die Kaskaden an Kamera und Sonne an. Jede Kaskade umschließt ihren Ausschnitt des
// Stumpfs mit einer Kugel, deren Radius nicht von der Blickrichtung abhängt; ihr Mittelpunkt
// rastet in 64-Bit-Weltkoordinaten auf ganze Texel ein. So bleiben Schattenkanten ruhig, wenn
// sich die Kamera dreht oder bewegt.
[[nodiscard]] std::array<Cascade, cascade_count>
fit_cascades(const CascadeView& view, math::Vec3 to_sun, const ShadowSettings& settings);

} // namespace ngine::render
