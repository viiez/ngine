// Tone Mapping vom HDR-Ziel in den Backbuffer (REND-11, R1.3): Belichtung, dann Khronos PBR
// Neutral, das Farben bis zur Kompression unverändert lässt (wie der glTF-Viewer es anbietet).
// Das Vollbild-Dreieck kommt aus fullscreen.slang.

#include "bindless.slang"

struct PushConstants
{
    uint hdr;
    float exposure;
    // 1, wenn das Ziel kein sRGB-Format hat und der Shader selbst kodieren muss.
    uint encode_srgb;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

float3 pbr_neutral(float3 color)
{
    let start_compression = 0.8 - 0.04;
    let desaturation = 0.15;
    let x = min(color.r, min(color.g, color.b));
    let offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    let peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression)
    {
        return color;
    }
    let d = 1.0 - start_compression;
    let new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    let g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return lerp(color, float3(new_peak), g);
}

float3 linear_to_srgb(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

[shader("fragment")]
float4 fragment_main(float4 position : SV_Position) : SV_Target
{
    let hdr = bindless_textures[NonUniformResourceIndex(push.hdr)].Load(int3(int2(position.xy), 0));
    var color = saturate(pbr_neutral(max(hdr.rgb * push.exposure, 0.0)));
    if (push.encode_srgb != 0)
    {
        color = linear_to_srgb(color);
    }
    return float4(color, 1.0);
}
