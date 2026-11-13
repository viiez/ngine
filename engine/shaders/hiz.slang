// Hi-Z (R3.5, ADR 0043): Tiefenpyramide für das Prüfen auf Verdeckung. Jede Stufe ist eine eigene
// R32-Textur; ein Texel hält die fernste Tiefe (umgekehrte Tiefe: den kleinsten Wert) des Bereichs,
// den es abdeckt. Stufe 0 hat die größte Zweierpotenz, die in die Tiefe passt, in jeder Achse;
// ein Texel deckt dort bis zu drei Pixel je Achse ab, darüber genau zwei.

#include "bindless.slang"

// Wie render::detail::HizPush.
struct HizPush
{
    // Quelle zum Lesen (Tiefe oder Stufe davor, sampled_index()) und Ziel (storage_index())
    uint source;
    uint target;
    uint2 source_size;
    uint2 target_size;
    uint2 reserved;
};

[[vk::push_constant]]
ConstantBuffer<HizPush> push;

float read_source(int2 texel)
{
    return bindless_textures[NonUniformResourceIndex(push.source)].Load(int3(texel, 0)).x;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.target_size.x || id.y >= push.target_size.y)
    {
        return;
    }
    // Bereich der Quelle, den das Texel abdeckt, aufgerundet nach außen.
    let begin = (id.xy * push.source_size) / push.target_size;
    let end = min(((id.xy + 1) * push.source_size + push.target_size - 1) / push.target_size,
                  push.source_size);
    var farthest = 1.0;
    for (uint y = begin.y; y < end.y; ++y)
    {
        for (uint x = begin.x; x < end.x; ++x)
        {
            farthest = min(farthest, read_source(int2(x, y)));
        }
    }
    storage_texture(push.target)[id.xy] = float4(farthest, 0.0, 0.0, 0.0);
}
