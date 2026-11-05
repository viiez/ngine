// Debug-Linien (REND-15, R1.4): Jede Linie wird im Vertex-Shader zu einem schmalen Rechteck aus
// zwei Dreiecken aufgezogen, sechs Eckpunkte pro Linie. Die Endpunkte liegen kamerarelativ und
// schon an der nahen Ebene abgeschnitten im Puffer.

#include "scene.slang"

// Wie render::GpuDebugLine.
struct Line
{
    float4 a;
    float4 b;
    float4 color;
};

struct PushConstants
{
    Line* lines;
    Frame* frame;
    float2 viewport;
    // Breite in Pixeln
    float width;
    uint reserved;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

float4 project(float3 p)
{
    let vp = push.frame.view_projection;
    let w = float4(p, 1.0);
    return float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w));
}

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    let line = push.lines[vertex_id / 6];
    // Ecke: welches Ende (0 oder 1) und welche Seite (-1 oder 1).
    static const float2 corners[6] = {
        float2(0.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
        float2(0.0, -1.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    let corner = corners[vertex_id % 6];

    let pa = project(line.a.xyz);
    let pb = project(line.b.xyz);
    // Richtung der Linie in Pixeln; senkrecht dazu wird aufgezogen.
    let delta = (pb.xy / pb.w - pa.xy / pa.w) * push.viewport;
    let len = length(delta);
    let dir = len > 1e-6 ? delta / len : float2(1.0, 0.0);
    let normal = float2(-dir.y, dir.x);
    // Ein Pixel sind 2 / viewport in Gerätekoordinaten; halbe Breite nach jeder Seite.
    let offset = normal * corner.y * push.width / push.viewport;

    var p = corner.x < 0.5 ? pa : pb;
    p.xy += offset * p.w;

    VertexOutput output;
    output.position = p;
    output.color = line.color.rgb;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
