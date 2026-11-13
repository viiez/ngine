// Voxel-Terrain im Renderer (R3.1): Netze der Chunks aus Surface Nets, wie ngine.voxel sie baut
// (voxel::Vertex). Gleiche Beleuchtung und Schatten wie pbr.slang. Seit R3.2 (VOX-07, ADR 0040)
// trägt jeder Eckpunkt Gewichte für die acht Plätze der Materialpalette seines Chunks; sie
// verlaufen über das Dreieck, und der Fragment-Shader mischt die Materialien danach. Jedes
// Material wird triplanar aufgetragen: Grundfarbe und Normalen aus drei Projektionen, gewichtet
// nach der Normale. Dazu Einstiege für die Schattenkarten und den ID-Puffer des Editors. Seit R3.5
// ist jeder Chunk eine Instanz mit Meshlets (driven.slang), gezeichnet über Mesh Shader oder
// indirekte Draws.

#include "driven.slang"

// Wie voxel::Vertex (32 Bytes): Position in Voxeln relativ zur Ecke des Chunks, Gewichte der
// Palettenplätze 0–3 (je 8 Bit), Normale nach außen, Gewichte der Plätze 4–7.
struct TerrainVertex
{
    float3 position;
    uint weights_low;
    float3 normal;
    uint weights_high;
};

[[vk::push_constant]]
ConstantBuffer<DrivenPushT<TerrainVertex>> push;

typealias Instance = InstanceT<TerrainVertex>;

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
    nointerpolation uint instance : INSTANCE0;
};

// Nur Position: Schattenkarten.
struct DepthOutput
{
    float4 position : SV_Position;
};

// Position und Kennung: ID-Puffer.
struct IdOutput
{
    float4 position : SV_Position;
    nointerpolation uint id : INSTANCE0;
};

float4 unpack_weights(uint packed)
{
    return float4(float(packed & 0xFF), float((packed >> 8) & 0xFF), float((packed >> 16) & 0xFF),
                  float(packed >> 24)) * (1.0 / 255.0);
}

uint palette_material(uint2 palette, uint slot)
{
    let word = slot < 4 ? palette.x : palette.y;
    return (word >> ((slot % 4) * 8)) & 0xFF;
}

float3 world_normal(float4 model[3], float3 n)
{
    // Spalten des linearen Teils; gleichmäßig skaliert, also genügt die Adjunkte.
    let c0 = float3(model[0].x, model[1].x, model[2].x);
    let c1 = float3(model[0].y, model[1].y, model[2].y);
    let c2 = float3(model[0].z, model[1].z, model[2].z);
    return cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z;
}

float4 to_clip(float3 world)
{
    let w = float4(world, 1.0);
    let vp = push.frame.view_projection;
    return float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w));
}

VertexOutput shade_vertex(Instance instance, uint instance_index, uint vertex_index)
{
    let v = instance.vertices[vertex_index];
    let world = transform_point(instance.model, v.position);
    VertexOutput output;
    output.position = to_clip(world);
    output.world = world;
    output.local = instance.offset.xyz + v.position * instance.offset.w;
    output.normal = world_normal(instance.model, v.normal);
    output.weights_low = unpack_weights(v.weights_low);
    output.weights_high = unpack_weights(v.weights_high);
    output.instance = instance_index;
    return output;
}

DepthOutput shadow_vertex(Instance instance, uint vertex_index)
{
    let v = instance.vertices[vertex_index];
    let p = float4(transform_point(instance.model, v.position), 1.0);
    let base = push.cascade * 3;
    let rows = push.frame.cascade_rows;
    DepthOutput output;
    output.position = float4(dot(rows[base], p), dot(rows[base + 1], p), dot(rows[base + 2], p), 1.0);
    return output;
}

IdOutput id_vertex(Instance instance, uint vertex_index)
{
    let v = instance.vertices[vertex_index];
    IdOutput output;
    output.position = to_clip(transform_point(instance.model, v.position));
    output.id = instance.id;
    return output;
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

// --- Vorwärts ---------------------------------------------------------------------------------

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID, uint instance : SV_VulkanInstanceID)
{
    return shade_vertex(push.instances[instance], instance, vertex_id);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void mesh_main(uint thread : SV_GroupThreadID, uint group : SV_GroupID, in payload Payload task,
               out vertices VertexOutput vertices[64], out indices uint3 triangles[124])
{
    let instance = push.instances[task.instance];
    let m = instance.meshlets[task.meshlets[group]];
    let vertex_count = meshlet_vertex_count(m);
    let triangle_count = meshlet_triangle_count(m);
    SetMeshOutputCounts(vertex_count, triangle_count);
    for (uint i = thread; i < vertex_count; i += mesh_threads)
    {
        vertices[i] = shade_vertex(instance, task.instance, instance.words[m.vertex_offset + i]);
    }
    for (uint i = thread; i < triangle_count; i += mesh_threads)
    {
        triangles[i] = unpack_triangle(instance.words[m.triangle_offset + i]);
    }
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    let n = normalize(input.normal);
    let palette = push.instances[input.instance].palette;
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
        let s = sample_material(push.terrain_materials[palette_material(palette, slot)], input.local,
                                n, blend);
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

// --- Schattenkarten: nur Tiefe aus der Sicht der Kaskade, ohne Fragment-Shader ---------------

[shader("vertex")]
DepthOutput shadow_vertex_main(uint vertex_id : SV_VertexID, uint instance : SV_VulkanInstanceID)
{
    return shadow_vertex(push.instances[instance], vertex_id);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void shadow_mesh_main(uint thread : SV_GroupThreadID, uint group : SV_GroupID, in payload Payload task,
                      out vertices DepthOutput vertices[64], out indices uint3 triangles[124])
{
    let instance = push.instances[task.instance];
    let m = instance.meshlets[task.meshlets[group]];
    let vertex_count = meshlet_vertex_count(m);
    let triangle_count = meshlet_triangle_count(m);
    SetMeshOutputCounts(vertex_count, triangle_count);
    for (uint i = thread; i < vertex_count; i += mesh_threads)
    {
        vertices[i] = shadow_vertex(instance, instance.words[m.vertex_offset + i]);
    }
    for (uint i = thread; i < triangle_count; i += mesh_threads)
    {
        triangles[i] = unpack_triangle(instance.words[m.triangle_offset + i]);
    }
}

// --- ID-Puffer für die Auswahl im Editor -----------------------------------------------------

[shader("vertex")]
IdOutput id_vertex_main(uint vertex_id : SV_VertexID, uint instance : SV_VulkanInstanceID)
{
    return id_vertex(push.instances[instance], vertex_id);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void id_mesh_main(uint thread : SV_GroupThreadID, uint group : SV_GroupID, in payload Payload task,
                  out vertices IdOutput vertices[64], out indices uint3 triangles[124])
{
    let instance = push.instances[task.instance];
    let m = instance.meshlets[task.meshlets[group]];
    let vertex_count = meshlet_vertex_count(m);
    let triangle_count = meshlet_triangle_count(m);
    SetMeshOutputCounts(vertex_count, triangle_count);
    for (uint i = thread; i < vertex_count; i += mesh_threads)
    {
        vertices[i] = id_vertex(instance, instance.words[m.vertex_offset + i]);
    }
    for (uint i = thread; i < triangle_count; i += mesh_threads)
    {
        triangles[i] = unpack_triangle(instance.words[m.triangle_offset + i]);
    }
}

[shader("fragment")]
uint id_fragment_main(IdOutput input) : SV_Target
{
    return input.id;
}
