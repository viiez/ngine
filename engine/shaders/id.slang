// ID-Puffer für die Auswahl im Editor (R2.2): zeichnet jedes Mesh mit der Kennung seiner Entity
// in eine R32_UINT-Textur. 0 heißt: nichts getroffen. Tiefe wie im Vorwärts-Pass (umgekehrt).

#include "scene.slang"

struct IdPush
{
    // Zeilen der affinen Matrix von Objekt- in kamerarelative Weltkoordinaten
    float4 model[3];
    Vertex* vertices;
    Frame* frame;
    uint id;
    uint reserved;
};

[[vk::push_constant]]
ConstantBuffer<IdPush> push;

[shader("vertex")]
float4 vertex_main(uint vertex_id : SV_VertexID) : SV_Position
{
    let v = push.vertices[vertex_id];
    let w = float4(transform_point(push.model, float3(v.px, v.py, v.pz)), 1.0);
    let vp = push.frame.view_projection;
    return float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w));
}

[shader("fragment")]
uint fragment_main() : SV_Target
{
    return push.id;
}
