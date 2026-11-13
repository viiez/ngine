// Schattenkarten der Sonne (REND-09, R1.4): nur Tiefe, aus der Sicht der Kaskade. Der
// Fragment-Shader läuft nur für Materialien mit Alpha-Maske; undurchsichtige zeichnet eine
// Pipeline ohne Fragment-Shader. Seit R3.5 GPU-getrieben (driven.slang): Meshlets außerhalb der
// Kaskade sortiert cull.slang aus.

#include "driven.slang"

[[vk::push_constant]]
ConstantBuffer<DrivenPushT<Vertex>> push;

typealias Instance = InstanceT<Vertex>;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint instance : INSTANCE0;
};

VertexOutput shade_vertex(Instance instance, uint instance_index, uint vertex_index)
{
    let v = instance.vertices[vertex_index];
    let world = transform_point(instance.model, float3(v.px, v.py, v.pz));
    let p = float4(world, 1.0);
    let base = push.cascade * 3;
    let rows = push.frame.cascade_rows;

    VertexOutput output;
    output.position = float4(dot(rows[base], p), dot(rows[base + 1], p), dot(rows[base + 2], p), 1.0);
    output.uv = float2(v.u, v.v);
    output.instance = instance_index;
    return output;
}

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
void fragment_main(VertexOutput input)
{
    let m = push.materials[push.instances[input.instance].material];
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
