// Vorwärts-Pass mit PBR-Materialien nach glTF Metallic-Roughness (REND-07, R1.3). Geometrie kommt
// über Vertex Pulling (ADR 0022), Materialien als Feld in einem Puffer mit bindlosen Texturindizes
// (ADR 0027). Gerechnet wird relativ zur Kamera (ADR 0012): Die Kamera steht im Ursprung. Seit
// R1.4 mit Schatten der Sonne über Kaskaden und einer Liste von Lichtern (ADR 0028), seit R3.5
// GPU-getrieben über Instanzen und Meshlets (driven.slang).

#include "driven.slang"

[[vk::push_constant]]
ConstantBuffer<DrivenPushT<Vertex>> push;

typealias Instance = InstanceT<Vertex>;

#include "lighting.slang"

struct VertexOutput
{
    float4 position : SV_Position;
    float3 world : POSITION0;
    float3 normal : NORMAL0;
    float4 tangent : TANGENT0;
    float2 uv : TEXCOORD0;
    nointerpolation uint instance : INSTANCE0;
};

// Ein Eckpunkt einer Instanz, gleich für Vertex- und Mesh-Shader.
VertexOutput shade_vertex(Instance instance, uint instance_index, uint vertex_index)
{
    let v = instance.vertices[vertex_index];
    let world = transform_point(instance.model, float3(v.px, v.py, v.pz));
    let w = float4(world, 1.0);

    // Spalten des linearen Teils; Normalen transformiert die Adjunkte (Kofaktoren), die auch bei
    // ungleicher Skalierung senkrecht bleibt.
    let c0 = float3(instance.model[0].x, instance.model[1].x, instance.model[2].x);
    let c1 = float3(instance.model[0].y, instance.model[1].y, instance.model[2].y);
    let c2 = float3(instance.model[0].z, instance.model[1].z, instance.model[2].z);
    let n = float3(v.nx, v.ny, v.nz);
    let t = float3(v.tx, v.ty, v.tz);

    VertexOutput output;
    let vp = push.frame.view_projection;
    output.position = float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w));
    output.world = world;
    output.normal = cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z;
    output.tangent = float4(c0 * t.x + c1 * t.y + c2 * t.z, v.tw);
    output.uv = float2(v.u, v.v);
    output.instance = instance_index;
    return output;
}

// Rückfallpfad und Durchsichtiges: indirekte bzw. einzelne Draws mit Indexpuffer; die Nummer der
// Instanz kommt als erste Instanz des Draws.
[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID, uint instance : SV_VulkanInstanceID)
{
    return shade_vertex(push.instances[instance], instance, vertex_id);
}

// Mesh-Shader-Pfad: ein Meshlet je Arbeitsgruppe, ausgewählt vom Task-Shader (cull.slang).
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

float4 sample_or(uint texture, uint sampler, float2 uv, float4 fallback)
{
    return texture == no_texture ? fallback : sample_texture(texture, sampler, uv);
}

[shader("fragment")]
float4 fragment_main(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target
{
    let m = push.materials[push.instances[input.instance].material];
    let uv = input.uv;

    var base = m.base_color * sample_or(m.textures.x, m.samplers.x, uv, float4(1.0));
    if (m.extra.z == 1 && base.a < m.emissive_cutoff.w)
    {
        discard;
    }
    let mr = sample_or(m.textures.y, m.samplers.y, uv, float4(1.0));
    let metallic = saturate(m.params.x * mr.b);
    let roughness = clamp(m.params.y * mr.g, 0.03, 1.0);

    var n = normalize(input.normal);
    if (!front)
    {
        n = -n; // Rückseite doppelseitiger Materialien
    }
    if (m.textures.z != no_texture)
    {
        var t = input.tangent.xyz - n * dot(n, input.tangent.xyz);
        if (dot(t, t) > 1e-12)
        {
            t = normalize(t);
            let b = cross(n, t) * (input.tangent.w < 0.0 ? -1.0 : 1.0);
            var local = sample_texture(m.textures.z, m.samplers.z, uv).xyz * 2.0 - 1.0;
            local.xy *= m.params.z;
            n = normalize(t * local.x + b * local.y + n * local.z);
        }
    }
    let occlusion = lerp(1.0, sample_or(m.textures.w, m.samplers.w, uv, float4(1.0)).r, m.params.w);
    let emissive = m.emissive_cutoff.xyz * sample_or(m.extra.x, m.extra.y, uv, float4(1.0)).rgb;

    let geometric = normalize(input.normal) * (front ? 1.0 : -1.0);
    var color = shade(input.world, n, geometric, base.rgb, metallic, roughness, occlusion);
    color += emissive;
    return float4(color, base.a);
}
