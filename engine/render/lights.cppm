module;

#include <optional>
#include <vector>

// Lichter der Welt in GPU-Daten (REND-09): Sonne für Schatten und Grundlicht, alle übrigen
// Lichter als Liste, die der Vorwärts-Pass für jedes Pixel durchgeht.
export module ngine.render:lights;

import ngine.core;
import ngine.math;
import ngine.world;
import :components;

export namespace ngine::render {

// Höchstzahl der Lichter in der Liste pro Frame; weitere werden mit Warnung übergangen.
inline constexpr u32 max_lights = 256;

enum class LightType : u32 { Directional = 0, Point = 1, Spot = 2 };

// Ein Licht, wie pbr.slang es liest (64 Bytes, nur float4).
struct GpuLight {
    // xyz: Position relativ zur Kamera, w: Reichweite (0 = unbegrenzt)
    math::Vec4 position_range;
    // rgb: Farbe mal Stärke, w: LightType
    math::Vec4 color_type;
    // xyz: Richtung, in die das Licht leuchtet (normiert), w: unbenutzt
    math::Vec4 direction;
    // x: Faktor, y: Versatz für den weichen Rand des Spotkegels (wie in KHR_lights_punctual)
    math::Vec4 spot;
};
static_assert(sizeof(GpuLight) == 64);

struct Sun {
    // Richtung zur Sonne (normiert).
    math::Vec3 to_sun{0.0f, 1.0f, 0.0f};
    // Farbe mal Stärke.
    math::Vec3 radiance{0.0f, 0.0f, 0.0f};
    bool cast_shadows = false;
};

struct LightList {
    // Das erste aktive DirectionalLight; ohne eines gibt es kein Sonnenlicht und keine Schatten.
    std::optional<Sun> sun;
    std::vector<GpuLight> lights;
};

// Sammelt die aktiven Lichter der Welt; Positionen relativ zu origin (der Kamera). WorldTransform
// muss aktuell sein.
[[nodiscard]] LightList collect_lights(world::World& world, math::DVec3 origin);

} // namespace ngine::render
