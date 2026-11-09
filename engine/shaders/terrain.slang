// Voxel-Terrain im Renderer (R3.1): Netze der Chunks aus Surface Nets, wie ngine.voxel sie baut
// (voxel::Vertex). Gleiche Beleuchtung und Schatten wie pbr.slang; die Farbe kommt aus dem
// Material jedes Eckpunkts und verläuft über das Dreieck. Triplanare Texturen folgen in R3.2
// (VOX-07). Dazu Einstiege für die Schattenkarten und den ID-Puffer des Editors.

#include "scene.slang"

// Wie voxel::Vertex (32 Bytes): Position in Voxeln relativ zur Ecke des Chunks, Material,
// Normale nach außen.
struct TerrainVertex
{
    float3 position;
    uint material;
    float3 normal;
    float reserved;
};

// Wie render::TerrainPush.
struct TerrainPush
{
    // Zeilen der affinen Matrix vom Chunk (Voxel) in kamerarelative Weltkoordinaten (Meter)
    float4 model[3];
    TerrainVertex* vertices;
    Frame* frame;
    // Nur für Schatten: die Kaskade, in die gezeichnet wird
    uint cascade;
    // Nur für den ID-Puffer: Kennung der Entity
    uint id;
};

[[vk::push_constant]]
ConstantBuffer<TerrainPush> push;

#include "lighting.slang"

struct VertexOutput
{
    float4 position : SV_Position;
    float3 world : POSITION0;
    float3 normal : NORMAL0;
    // Grundfarbe pro Eckpunkt (linear), interpoliert: Materialgrenzen verlaufen weich.
    float3 albedo : COLOR0;
};

// Grundfarbe der Materialien (linear). 1 Gras, 2 Fels, 3 Erde, 4 Sand.
float3 material_color(uint material, float3 normal)
{
    let grass = float3(0.045, 0.13, 0.02);
    let rock = float3(0.16, 0.15, 0.135);
    let earth = float3(0.15, 0.075, 0.03);
    let sand = float3(0.45, 0.38, 0.24);
    switch (material)
    {
    case 1:
        // Gras rutscht an steilen Stellen ab, darunter kommt Fels zum Vorschein.
        return lerp(rock, grass, smoothstep(0.55, 0.8, normal.y));
    case 2:
        return rock;
    case 3:
        return earth;
    case 4:
        return sand;
    default:
        return float3(1.0, 0.0, 1.0);
    }
}

float3 world_normal(float3 n)
{
    // Spalten des linearen Teils; gleichmäßig skaliert, also genügt die Adjunkte.
    let c0 = float3(push.model[0].x, push.model[1].x, push.model[2].x);
    let c1 = float3(push.model[0].y, push.model[1].y, push.model[2].y);
    let c2 = float3(push.model[0].z, push.model[1].z, push.model[2].z);
    return cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z;
}

float4 to_clip(float3 world)
{
    let w = float4(world, 1.0);
    let vp = push.frame.view_projection;
    return float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w));
}

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    let v = push.vertices[vertex_id];
    let world = transform_point(push.model, v.position);
    VertexOutput output;
    output.position = to_clip(world);
    output.world = world;
    output.normal = world_normal(v.normal);
    output.albedo = material_color(v.material, v.normal);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    let n = normalize(input.normal);
    let color = shade(input.world, n, n, input.albedo, 0.0, 0.92, 1.0);
    return float4(color, 1.0);
}

// Schattenkarten: nur Tiefe aus der Sicht der Kaskade, ohne Fragment-Shader.
[shader("vertex")]
float4 shadow_vertex_main(uint vertex_id : SV_VertexID) : SV_Position
{
    let v = push.vertices[vertex_id];
    let p = float4(transform_point(push.model, v.position), 1.0);
    let base = push.cascade * 3;
    let rows = push.frame.cascade_rows;
    return float4(dot(rows[base], p), dot(rows[base + 1], p), dot(rows[base + 2], p), 1.0);
}

// ID-Puffer für die Auswahl im Editor.
[shader("vertex")]
float4 id_vertex_main(uint vertex_id : SV_VertexID) : SV_Position
{
    let v = push.vertices[vertex_id];
    return to_clip(transform_point(push.model, v.position));
}

[shader("fragment")]
uint id_fragment_main() : SV_Target
{
    return push.id;
}
