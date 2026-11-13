// ID-Puffer für die Auswahl im Editor (R2.2): zeichnet jedes Mesh mit der Kennung seiner Entity
// in eine R32_UINT-Textur. 0 heißt: nichts getroffen. Tiefe wie im Vorwärts-Pass (umgekehrt).
// Seit R3.5 GPU-getrieben (driven.slang); die Kennung steht in der Instanz.

#include "driven.slang"

[[vk::push_constant]]
ConstantBuffer<DrivenPushT<Vertex>> push;

typealias Instance = InstanceT<Vertex>;

struct VertexOutput
{
    float4 position : SV_Position;
    nointerpolation uint id : INSTANCE0;
};

VertexOutput shade_vertex(Instance instance, uint vertex_index)
{
    let v = instance.vertices[vertex_index];
    let w = float4(transform_point(instance.model, float3(v.px, v.py, v.pz)), 1.0);
    let vp = push.frame.view_projection;
    VertexOutput output;
    output.position = float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w));
    output.id = instance.id;
    return output;
}

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID, uint instance : SV_VulkanInstanceID)
{
    return shade_vertex(push.instances[instance], vertex_id);
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
        vertices[i] = shade_vertex(instance, instance.words[m.vertex_offset + i]);
    }
    for (uint i = thread; i < triangle_count; i += mesh_threads)
    {
        triangles[i] = unpack_triangle(instance.words[m.triangle_offset + i]);
    }
}

[shader("fragment")]
uint fragment_main(VertexOutput input) : SV_Target
{
    return input.id;
}
