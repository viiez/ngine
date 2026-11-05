// Editor-UI (EDIT-01, R2.1): Dreiecke aus der Zeichenliste von Dear ImGui. Die Eckpunkte liest der
// Vertex-Shader selbst aus einem Puffer (ADR 0022); Textur und Sampler kommen bindlos.
//
// Farben von ImGui sind sRGB-kodiert. Der Shader rechnet sie in linear um; ein sRGB-Ziel kodiert
// beim Schreiben wieder. Bei einem Unorm-Ziel kodiert der Shader selbst (wie tonemap.slang).

#include "bindless.slang"

// Wie editor_ui::UiVertex (20 Bytes, nur 4-Byte-Felder, damit es keine Lücken gibt).
struct Vertex
{
    float x, y;
    float u, v;
    // RGBA, 8 Bit je Kanal, R im niedrigsten Byte
    uint color;
};

struct PushConstants
{
    Vertex* vertices;
    // Von Pixeln (Ursprung oben links) in Clip-Koordinaten: p * scale + translate.
    float2 scale;
    float2 translate;
    uint texture;
    uint sampler;
    // 1, wenn das Ziel kein sRGB-Format hat und der Shader selbst kodieren muss.
    uint encode_srgb;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float3 srgb_to_linear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

float3 linear_to_srgb(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    let vertex = push.vertices[vertex_id];
    let packed = vertex.color;
    let color = float4(float(packed & 0xFF), float((packed >> 8) & 0xFF),
                       float((packed >> 16) & 0xFF), float(packed >> 24)) / 255.0;
    VertexOutput output;
    output.position = float4(float2(vertex.x, vertex.y) * push.scale + push.translate, 0.0, 1.0);
    output.uv = float2(vertex.u, vertex.v);
    output.color = float4(srgb_to_linear(color.rgb), color.a);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    var color = input.color * sample_texture(push.texture, push.sampler, input.uv);
    if (push.encode_srgb != 0)
    {
        color.rgb = linear_to_srgb(saturate(color.rgb));
    }
    return color;
}
