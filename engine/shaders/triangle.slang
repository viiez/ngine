// Das erste Dreieck: Positionen und Farben stehen direkt im Shader,
// es gibt noch keine Vertex-Buffer. Die y-Achse zeigt nach oben.

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

static const float2 positions[3] = {
    float2(0.0, 0.6),
    float2(-0.6, -0.5),
    float2(0.6, -0.5),
};

static const float3 colors[3] = {
    float3(1.0, 0.25, 0.2),
    float3(0.2, 0.9, 0.3),
    float3(0.2, 0.4, 1.0),
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.color = colors[vertex_id];
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
