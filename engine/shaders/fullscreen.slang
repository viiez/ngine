// Zeichnet eine Textur über das ganze Ziel: ein Dreieck, das den Bildschirm bedeckt, ohne Puffer.
// Grundlage für Kopien zwischen Zielen und später das Tone Mapping (R1.3).

#include "bindless.slang"

struct PushConstants
{
    uint texture;
    uint sampler;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

struct VertexOutput
{
    float4 position : SV_Position;
    // (0, 0) oben links, (1, 1) unten rechts, wie Texturkoordinaten.
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    // Eckpunkte (-1, -1), (3, -1), (-1, 3): Das Dreieck deckt das Rechteck von -1 bis 1 ab.
    let corner = float2(float((vertex_id << 1) & 2), float(vertex_id & 2));
    VertexOutput output;
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    // Die y-Achse des Clip-Raums zeigt nach oben, die der Textur nach unten.
    output.uv = float2(corner.x, 1.0 - corner.y);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    return sample_texture(push.texture, push.sampler, input.uv);
}
