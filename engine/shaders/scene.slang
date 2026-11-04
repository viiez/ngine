// Gemeinsame Daten der Szenen-Shader (pbr.slang, shadow.slang, debug.slang): Eckpunkte,
// Materialien, Lichter und die Daten des Frames, wie der Renderer sie in Puffer schreibt. Alle
// Strukturen bestehen aus float4/uint4 oder sind so ausgerichtet, dass die Ausrichtungsregeln
// keine Rolle spielen.

#include "bindless.slang"

static const uint no_texture = 0xFFFFFFFF;
static const uint cascade_count = 4;

// Wie asset::Vertex: 12 Floats ohne Lücken.
struct Vertex
{
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz, tw;
    float u, v;
};

// Wie render::GpuMaterial (96 Bytes).
struct Material
{
    float4 base_color;
    // xyz: Emission, w: Schwelle der Alpha-Maske
    float4 emissive_cutoff;
    // x: Metallic, y: Roughness, z: Stärke der Normalenkarte, w: Stärke der Okklusion
    float4 params;
    // Texturen: Grundfarbe, Metallic-Roughness, Normalen, Okklusion
    uint4 textures;
    // Sampler in derselben Reihenfolge
    uint4 samplers;
    // x: Emissionstextur, y: ihr Sampler, z: Alpha-Modus (0 undurchsichtig, 1 Maske, 2 Blend)
    uint4 extra;
};

// Wie render::GpuLight (64 Bytes).
struct Light
{
    // xyz: Position relativ zur Kamera, w: Reichweite (0 = unbegrenzt)
    float4 position_range;
    // rgb: Farbe mal Stärke, w: Art (0 gerichtet, 1 Punkt, 2 Spot)
    float4 color_type;
    // xyz: Richtung, in die das Licht leuchtet
    float4 direction;
    // x: Faktor, y: Versatz für den Rand des Spotkegels
    float4 spot;
};

// Wie render::GpuFrame (448 Bytes).
struct Frame
{
    // Zeilen der Matrix von kamerarelativen Welt- in Clip-Koordinaten
    float4 view_projection[4];
    // xyz: Richtung zur Sonne (normiert)
    float4 sun_direction;
    // rgb: Farbe mal Stärke der Sonne
    float4 sun_color;
    float4 sky_color;
    float4 ground_color;
    // xyz: Blickrichtung der Kamera
    float4 camera_forward;
    // Ende jeder Kaskade und Beginn ihres Übergangs in die nächste (Entfernung entlang der
    // Blickrichtung), Kantenlänge eines Texels in Metern
    float4 cascade_far;
    float4 cascade_blend;
    float4 cascade_texel;
    // Je drei Zeilen der Matrix von kamerarelativen Welt- in Schattenkoordinaten
    float4 cascade_rows[12];
    // Bindlose Indizes der Schattenkarten
    uint4 shadow_maps;
    // x: Zahl der Lichter, y: Schatten an, z: Kaskaden einfärben, w: Sampler für Schattenkarten
    uint4 options;
    // x: Versatz entlang der Normale in Texeln, y: 1 / Auflösung, z: Auflösung
    float4 shadow_params;
    Light* lights;
    uint2 reserved;
};

// Wie render::DrawPush.
struct DrawPush
{
    // Zeilen der affinen Matrix von Objekt- in kamerarelative Weltkoordinaten
    float4 model[3];
    Vertex* vertices;
    Frame* frame;
    Material* materials;
    uint material;
    // Nur für Schatten: die Kaskade, in die gezeichnet wird
    uint cascade;
};

float3 transform_point(float4 rows[3], float3 position)
{
    let p = float4(position, 1.0);
    return float3(dot(rows[0], p), dot(rows[1], p), dot(rows[2], p));
}
