// Voxel-Terrain im Renderer (R3.1): Netze der Chunks aus Surface Nets, wie ngine.voxel sie baut
// (voxel::Vertex). Gleiche Beleuchtung und Schatten wie pbr.slang. Seit R3.2 (VOX-07, ADR 0040)
// trägt jeder Eckpunkt Gewichte für die acht Plätze der Materialpalette seines Chunks; sie
// verlaufen über das Dreieck, und der Fragment-Shader mischt die Materialien danach. Jedes
// Material wird triplanar aufgetragen: Grundfarbe und Normalen aus drei Projektionen, gewichtet
// nach der Normale. Dazu Einstiege für die Schattenkarten und den ID-Puffer des Editors.

#include "scene.slang"

// Wie voxel::Vertex (32 Bytes): Position in Voxeln relativ zur Ecke des Chunks, Gewichte der
// Palettenplätze 0–3 (je 8 Bit), Normale nach außen, Gewichte der Plätze 4–7.
struct TerrainVertex
{
    float3 position;
    uint weights_low;
    float3 normal;
    uint weights_high;
};

// Wie render::GpuTerrainMaterial (48 Bytes).
struct TerrainMaterial
{
    // xyz: Farbe (linear, Faktor auf die Textur), w: Roughness
    float4 color;
    // x: Meter pro Wiederholung, y: Stärke der Normalen
    float4 params;
    // x: Grundfarbe, y: Normalen, z: Sampler, w: frei
    uint4 textures;
};

// Wie render::TerrainPush (104 Bytes).
struct TerrainPush
{
    // Zeilen der affinen Matrix vom Chunk (Voxel) in kamerarelative Weltkoordinaten (Meter)
    float4 model[3];
    // xyz: Ecke des Chunks relativ zum Terrain in Metern, w: Meter pro Voxel
    float4 offset;
    TerrainVertex* vertices;
    Frame* frame;
    TerrainMaterial* materials;
    // Nur für Schatten: die Kaskade, in die gezeichnet wird
    uint cascade;
    // Nur für den ID-Puffer: Kennung der Entity
    uint id;
    // Materialnummern der acht Palettenplätze, vier je Wort
    uint2 palette;
};

[[vk::push_constant]]
ConstantBuffer<TerrainPush> push;

#include "lighting.slang"

struct VertexOutput
{
    float4 position : SV_Position;
    float3 world : POSITION0;
    // Lage im Terrain in Metern, an der die Texturen haften
    float3 local : POSITION1;
    float3 normal : NORMAL0;
    // Gewichte der Palettenplätze, interpoliert
    float4 weights_low : COLOR0;
    float4 weights_high : COLOR1;
};

float4 unpack_weights(uint packed)
{
    return float4(float(packed & 0xFF), float((packed >> 8) & 0xFF), float((packed >> 16) & 0xFF),
                  float(packed >> 24)) * (1.0 / 255.0);
}

uint palette_material(uint slot)
{
    let word = slot < 4 ? push.palette.x : push.palette.y;
    return (word >> ((slot % 4) * 8)) & 0xFF;
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

// Ergebnis eines Materials an einem Punkt.
struct Surface
{
    float3 albedo;
    float3 normal;
    float roughness;
};

// Triplanar: drei Projektionen entlang der Achsen, gewichtet nach der Normale. Die Normalen der
// Texturen kommen über das Verfahren „UDN“ in die Welt (Ben Golus, „Normal Mapping for a
// Triplanar Shader“), mit gespiegelten Koordinaten auf den abgewandten Seiten.
Surface sample_material(TerrainMaterial m, float3 p, float3 n, float3 blend)
{
    Surface s;
    s.roughness = m.color.w;
    s.normal = n;
    let uv_x = p.zy / m.params.x;
    let uv_y = p.xz / m.params.x;
    let uv_z = p.xy / m.params.x;
    let axis = float3(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0, n.z < 0.0 ? -1.0 : 1.0);
    let ux = float2(uv_x.x * axis.x, uv_x.y);
    let uy = float2(uv_y.x * axis.y, uv_y.y);
    let uz = float2(uv_z.x * -axis.z, uv_z.y);

    var albedo = float3(1.0);
    if (m.textures.x != no_texture)
    {
        albedo = sample_texture(m.textures.x, m.textures.z, ux).rgb * blend.x +
                 sample_texture(m.textures.x, m.textures.z, uy).rgb * blend.y +
                 sample_texture(m.textures.x, m.textures.z, uz).rgb * blend.z;
    }
    s.albedo = albedo * m.color.xyz;

    if (m.textures.y != no_texture)
    {
        var tx = sample_texture(m.textures.y, m.textures.z, ux).xyz * 2.0 - 1.0;
        var ty = sample_texture(m.textures.y, m.textures.z, uy).xyz * 2.0 - 1.0;
        var tz = sample_texture(m.textures.y, m.textures.z, uz).xyz * 2.0 - 1.0;
        tx.xy *= m.params.y;
        ty.xy *= m.params.y;
        tz.xy *= m.params.y;
        tx.x *= axis.x;
        ty.x *= axis.y;
        tz.x *= -axis.z;
        // UDN: Abweichung der Textur auf die Normale der Oberfläche legen.
        let wx = float3(tx.xy + n.zy, n.x);
        let wy = float3(ty.xy + n.xz, n.y);
        let wz = float3(tz.xy + n.xy, n.z);
        s.normal = normalize(wx.zyx * blend.x + wy.xzy * blend.y + wz.xyz * blend.z);
    }
    return s;
}

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    let v = push.vertices[vertex_id];
    let world = transform_point(push.model, v.position);
    VertexOutput output;
    output.position = to_clip(world);
    output.world = world;
    output.local = push.offset.xyz + v.position * push.offset.w;
    output.normal = world_normal(v.normal);
    output.weights_low = unpack_weights(v.weights_low);
    output.weights_high = unpack_weights(v.weights_high);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    let n = normalize(input.normal);
    // Gewichte der drei Projektionen: scharf zur Hauptachse hin, Summe 1.
    var blend = pow(abs(n), float3(4.0));
    blend /= max(blend.x + blend.y + blend.z, 1e-5);

    float weights[8] = {
        input.weights_low.x,  input.weights_low.y,  input.weights_low.z,  input.weights_low.w,
        input.weights_high.x, input.weights_high.y, input.weights_high.z, input.weights_high.w,
    };
    // Alle Materialien mit nennenswertem Gewicht mischen; winzige Anteile weglassen spart Zugriffe
    // und fällt nicht auf, weil die Gewichte stetig gegen 0 gehen.
    var albedo = float3(0.0);
    var normal = float3(0.0);
    var roughness = 0.0;
    var total = 0.0;
    for (uint slot = 0; slot < 8; ++slot)
    {
        let w = weights[slot];
        if (w < 1.0 / 64.0)
        {
            continue;
        }
        let s = sample_material(push.materials[palette_material(slot)], input.local, n, blend);
        albedo += s.albedo * w;
        normal += s.normal * w;
        roughness += s.roughness * w;
        total += w;
    }
    if (total <= 0.0)
    {
        albedo = float3(1.0, 0.0, 1.0);
        normal = n;
        roughness = 1.0;
        total = 1.0;
    }
    albedo /= total;
    roughness /= total;
    let shading = dot(normal, normal) > 1e-8 ? normalize(normal) : n;
    let color = shade(input.world, shading, n, albedo, 0.0, clamp(roughness, 0.03, 1.0), 1.0);
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
