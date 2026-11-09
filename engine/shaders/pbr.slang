// Vorwärts-Pass mit PBR-Materialien nach glTF Metallic-Roughness (REND-07, R1.3). Geometrie kommt
// über Vertex Pulling (ADR 0022), Materialien als Feld in einem Puffer mit bindlosen Texturindizes
// (ADR 0027). Gerechnet wird relativ zur Kamera (ADR 0012): Die Kamera steht im Ursprung. Seit
// R1.4 mit Schatten der Sonne über Kaskaden und einer Liste von Lichtern (ADR 0028).

#include "scene.slang"

[[vk::push_constant]]
ConstantBuffer<DrawPush> push;

#include "lighting.slang"

struct VertexOutput
{
    float4 position : SV_Position;
    float3 world : POSITION0;
    float3 normal : NORMAL0;
    float4 tangent : TANGENT0;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    let v = push.vertices[vertex_id];
    let world = transform_point(push.model, float3(v.px, v.py, v.pz));
    let w = float4(world, 1.0);

    // Spalten des linearen Teils; Normalen transformiert die Adjunkte (Kofaktoren), die auch bei
    // ungleicher Skalierung senkrecht bleibt.
    let c0 = float3(push.model[0].x, push.model[1].x, push.model[2].x);
    let c1 = float3(push.model[0].y, push.model[1].y, push.model[2].y);
    let c2 = float3(push.model[0].z, push.model[1].z, push.model[2].z);
    let n = float3(v.nx, v.ny, v.nz);
    let t = float3(v.tx, v.ty, v.tz);

    VertexOutput output;
    let vp = push.frame.view_projection;
    output.position = float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w));
    output.world = world;
    output.normal = cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z;
    output.tangent = float4(c0 * t.x + c1 * t.y + c2 * t.z, v.tw);
    output.uv = float2(v.u, v.v);
    return output;
}

float4 sample_or(uint texture, uint sampler, float2 uv, float4 fallback)
{
    return texture == no_texture ? fallback : sample_texture(texture, sampler, uv);
}

[shader("fragment")]
float4 fragment_main(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target
{
    let m = push.materials[push.material];
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
