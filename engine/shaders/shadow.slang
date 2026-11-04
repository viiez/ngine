// Schattenkarten der Sonne (REND-09, R1.4): nur Tiefe, aus der Sicht der Kaskade. Der
// Fragment-Shader läuft nur für Materialien mit Alpha-Maske; undurchsichtige zeichnet eine
// Pipeline ohne Fragment-Shader.

#include "scene.slang"

[[vk::push_constant]]
ConstantBuffer<DrawPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    let v = push.vertices[vertex_id];
    let world = transform_point(push.model, float3(v.px, v.py, v.pz));
    let p = float4(world, 1.0);
    let base = push.cascade * 3;
    let rows = push.frame.cascade_rows;

    VertexOutput output;
    output.position = float4(dot(rows[base], p), dot(rows[base + 1], p), dot(rows[base + 2], p), 1.0);
    output.uv = float2(v.u, v.v);
    return output;
}

[shader("fragment")]
void fragment_main(VertexOutput input)
{
    let m = push.materials[push.material];
    var alpha = m.base_color.a;
    if (m.textures.x != no_texture)
    {
        alpha *= sample_texture(m.textures.x, m.samplers.x, input.uv).a;
    }
    if (alpha < m.emissive_cutoff.w)
    {
        discard;
    }
}
